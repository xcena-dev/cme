// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// rdma_lock.cpp -- 2-node RDMA hardware-atomic remote lock microbenchmark.
//
// Not CME. This is the external reference point Section 11.3 of the design record
// compares against, so it holds no cme header and links no cme target.
//
// Server registers D independent 8-byte lock words (page-spaced 4 KiB apart) in one MR
// with REMOTE_ATOMIC. Clients each own an RC QP and acquire a lock via
// IBV_WR_ATOMIC_CMP_AND_SWP (0 -> id), trivial CS, release (id -> 0). QP/MR info is
// exchanged over a plain TCP socket (RoCEv2 GID addressing).
//
// Modes:
//   (default) performance -- empty critical section, per-acquire latency
//     (mean/p50/p90/p99 us) + throughput, swept over peers x domains.
//   --verify -- correctness soak: the critical section does a software (NON-atomic)
//     RDMA_READ / ++ / RDMA_WRITE on a per-domain counter. A correct lock yields
//     counter[d] == peers*iters; any lost update means the lock let two peers in at
//     once. Prints VERIFY: PASS/FAIL. Separate from the perf path (no counters there).
//   Acquire retry policy: --backoff (default) = exponential backoff w/ jitter;
//     --blind = immediate re-CAS (documented O(N^2) worst case). Backoff is default
//     because it collapses the D=1 high-contention blowup (see rdma_lock.sh).
//   --ticket: DSLR-style FAA TICKET lock (exclusive-only core) -- an ORDERED/fair
//     RDMA lock. Per domain: ticket[d] (next number) + serving[d] (now serving).
//     ACQUIRE = my=FAA(ticket[d]); spin RDMA_READ(serving[d]) until ==my. RELEASE =
//     FAA(serving[d]). FCFS, bounded (<= N-1 turns), no re-CAS. Overrides backoff/blind.
//     NOT full DSLR -- this is only the FAA-ticket ordering core DSLR builds on:
//       - exclusive locks only (no shared/update lock modes),
//       - no multi-slot leasing (DSLR's long-read mechanism),
//       - plain ticket/serving pair (no packed shared/exclusive conflict counters).
//     Read the numbers as "FAA-ticket ordering core", not "DSLR measured".

#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
// posix_memalign is POSIX, which <cstdlib> does not declare.
#include <stdlib.h>  // NOLINT(modernize-deprecated-headers)
#include <sys/socket.h>
#include <sys/types.h>
// clock_gettime and CLOCK_MONOTONIC are POSIX, which <ctime> does not declare.
#include <time.h>  // NOLINT(modernize-deprecated-headers)
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

// Domains are page-spaced so that two locks never share a NIC atomic slot.
constexpr std::uint64_t DomainStrideBytes = 4096;
constexpr std::uint16_t TcpPort = 18600;

// Four regions in the server's MR, each holding `domains` words at DomainStrideBytes.
constexpr std::uint64_t RegionCount = 4;

constexpr std::size_t GidBytes = 16;
constexpr std::size_t WordBytes = 8;
constexpr std::uint64_t PageBytes = 4096;

// One work request in flight per thread, so the queues only need room for a handful.
constexpr std::int32_t QueueDepth = 16;
constexpr std::int32_t CqDepth = 16;
constexpr std::uint8_t MaxRdAtomic = 16;
constexpr std::uint8_t QpTimeout = 14;
constexpr std::uint8_t RetryCount = 7;
constexpr std::uint8_t RnrRetry = 7;
constexpr std::uint8_t MinRnrTimer = 12;
constexpr std::uint8_t HopLimit = 1;

// Distinct per side, so a stale packet from one direction cannot be accepted by the other.
constexpr std::uint32_t ServerPsnBase = 0x1234;
constexpr std::uint32_t ClientPsnBase = 0x4321;

constexpr std::int32_t ListenBacklog = 128;
constexpr std::uint32_t ConnectAttempts = 100;
constexpr std::uint32_t ConnectRetryUs = 50000;

// Acquire backoff: start here, double per failed CAS, stop growing at the cap.
constexpr double BackoffStartUs = 1.0;
constexpr double BackoffCapUs = 128.0;

// Jitter in [0.5, 1.5), so N peers that failed together do not retry together.
constexpr std::uint32_t JitterMask = 0xffff;
constexpr double JitterDivisor = 65536.0;
constexpr double JitterFloor = 0.5;

constexpr std::uint32_t SeedMultiplier = 2654435761u;
constexpr double UsPerSecond = 1e6;
constexpr double NsPerUs = 1e3;

constexpr double MedianFraction = 0.50;
constexpr double P90Fraction = 0.90;
constexpr double P99Fraction = 0.99;

// Print every domain's counter only when the list is short enough to read.
constexpr std::uint32_t VerifyEchoDomainLimit = 8;

constexpr std::uint32_t DefaultPeers = 1;
constexpr std::uint32_t DefaultDomains = 1;
constexpr std::uint32_t DefaultIterations = 2000;
constexpr std::uint32_t DefaultWarmup = 100;
constexpr std::uint32_t DefaultQps = 1;
constexpr std::uint8_t DefaultGidIndex = 3;  // RoCEv2 IPv4
constexpr std::uint8_t PortNum = 1;

// nullptr = take whatever HCA this host has. A name here would be one machine's card, and
// this file links nothing from the harness on purpose, so it cannot read config.yaml
// itself; rdma_lock.sh passes --dev from that file instead.
const char* g_device = nullptr;
std::uint8_t g_gidIndex = DefaultGidIndex;
bool g_useBackoff = true;     // false = blind re-CAS
bool g_useTicket = false;     // overrides the CAS policies entirely
double g_ticketSpinUs = 0.7;  // inter-poll spin between serving[] reads

struct WireQp_t  // exchanged both directions
{
    std::uint32_t qpn;
    std::uint32_t psn;
    std::uint8_t gid[GidBytes];
};

struct WireMr_t  // server -> client
{
    std::uint64_t addr;
    std::uint32_t rkey;
    std::uint32_t domains;
    std::uint32_t stride;
};

// Where each of the four regions starts. The lock words are at the MR base; the rest are
// only touched by the mode that needs them.
struct RegionBases_t
{
    std::uint64_t lock{0};
    std::uint64_t counter{0};  // --verify
    std::uint64_t ticket{0};   // --ticket, next number
    std::uint64_t serving{0};  // --ticket, now serving
};

[[nodiscard]] RegionBases_t computeRegionBases(const WireMr_t& remote)
{
    const std::uint64_t span = static_cast<std::uint64_t>(remote.domains) * remote.stride;
    RegionBases_t bases;
    bases.lock = remote.addr;
    bases.counter = remote.addr + span;
    bases.ticket = remote.addr + 2 * span;
    bases.serving = remote.addr + 3 * span;
    return bases;
}

[[nodiscard]] double readClockUs()
{
    struct timespec now = {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) * UsPerSecond + static_cast<double>(now.tv_nsec) / NsPerUs;
}

// Busy-wait, because the windows here are single-digit microseconds and a nanosleep would
// hand the core away for longer than the wait itself.
void spinUntil(double deadlineUs)
{
    while (readClockUs() < deadlineUs)
    {
    }
}

// Per-thread xorshift32. A shared RNG would put a lock in the middle of the thing being
// measured.
[[nodiscard]] std::uint32_t nextRandom(std::uint32_t& state)
{
    std::uint32_t value = state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    state = value;
    return value;
}

// ── verbs setup ─────────────────────────────────────────────────────────

[[nodiscard]] ibv_context* openDevice()
{
    int deviceCount = 0;
    ibv_device** list = ::ibv_get_device_list(&deviceCount);
    if (list == nullptr)
    {
        std::perror("get_device_list");
        std::exit(1);
    }

    ibv_device* device = nullptr;
    if (g_device != nullptr)
    {
        for (int index = 0; index < deviceCount; ++index)
        {
            if (std::strcmp(::ibv_get_device_name(list[index]), g_device) == 0)
            {
                device = list[index];
                break;
            }
        }
        if (device == nullptr)
        {
            std::fprintf(stderr, "device %s not found\n", g_device);
            std::exit(1);
        }
    }
    else
    {
        if (deviceCount < 1)
        {
            std::fprintf(stderr, "no RDMA device on this host\n");
            std::exit(1);
        }
        device = list[0];
        std::fprintf(stderr, "using RDMA device %s (pass --dev to choose)\n",
                     ::ibv_get_device_name(device));
    }

    ibv_context* context = ::ibv_open_device(device);
    if (context == nullptr)
    {
        std::fprintf(stderr, "open_device failed\n");
        std::exit(1);
    }
    ::ibv_free_device_list(list);
    return context;
}

[[nodiscard]] int moveQpToRtr(ibv_qp* queuePair, const WireQp_t& remote)
{
    union ibv_gid remoteGid;
    std::memcpy(&remoteGid, remote.gid, GidBytes);

    ibv_qp_attr attr;
    std::memset(&attr, 0, sizeof attr);
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote.qpn;
    attr.rq_psn = remote.psn;
    attr.max_dest_rd_atomic = MaxRdAtomic;
    attr.min_rnr_timer = MinRnrTimer;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = remoteGid;
    attr.ah_attr.grh.sgid_index = g_gidIndex;
    attr.ah_attr.grh.hop_limit = HopLimit;
    attr.ah_attr.grh.traffic_class = 0;
    attr.ah_attr.dlid = 0;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = PortNum;

    const int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                      IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    const int result = ::ibv_modify_qp(queuePair, &attr, flags);
    if (result != 0)
    {
        std::fprintf(stderr, "RTR failed: %s\n", std::strerror(result));
    }
    return result;
}

[[nodiscard]] int moveQpToRts(ibv_qp* queuePair, std::uint32_t myPsn)
{
    ibv_qp_attr attr;
    std::memset(&attr, 0, sizeof attr);
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = QpTimeout;
    attr.retry_cnt = RetryCount;
    attr.rnr_retry = RnrRetry;
    attr.sq_psn = myPsn;
    attr.max_rd_atomic = MaxRdAtomic;

    const int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                      IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    const int result = ::ibv_modify_qp(queuePair, &attr, flags);
    if (result != 0)
    {
        std::fprintf(stderr, "RTS failed: %s\n", std::strerror(result));
    }
    return result;
}

// One RC queue pair on @context, left in INIT with remote access enabled.
[[nodiscard]] ibv_qp* createQueuePair(ibv_context* context, ibv_pd* protectionDomain, ibv_cq** cqOut)
{
    ibv_cq* completionQueue = ::ibv_create_cq(context, CqDepth, nullptr, nullptr, 0);
    if (completionQueue == nullptr)
    {
        std::perror("create_cq");
        std::exit(1);
    }

    ibv_qp_init_attr initAttr;
    std::memset(&initAttr, 0, sizeof initAttr);
    initAttr.send_cq = completionQueue;
    initAttr.recv_cq = completionQueue;
    initAttr.qp_type = IBV_QPT_RC;
    initAttr.cap.max_send_wr = QueueDepth;
    initAttr.cap.max_recv_wr = QueueDepth;
    initAttr.cap.max_send_sge = 1;
    initAttr.cap.max_recv_sge = 1;

    ibv_qp* queuePair = ::ibv_create_qp(protectionDomain, &initAttr);
    if (queuePair == nullptr)
    {
        std::perror("create_qp");
        std::exit(1);
    }

    ibv_qp_attr attr;
    std::memset(&attr, 0, sizeof attr);
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = PortNum;
    attr.qp_access_flags =
        IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
    static_cast<void>(::ibv_modify_qp(
        queuePair, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));

    *cqOut = completionQueue;
    return queuePair;
}

// ── TCP rendezvous ──────────────────────────────────────────────────────

[[nodiscard]] int listenOnTcp()
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    static_cast<void>(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one));

    sockaddr_in address;
    std::memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = ::htons(TcpPort);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
    {
        std::perror("bind");
        std::exit(1);
    }
    if (::listen(listener, ListenBacklog) != 0)
    {
        std::perror("listen");
        std::exit(1);
    }
    return listener;
}

[[nodiscard]] int connectOverTcp(const char* serverIp)
{
    const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address;
    std::memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = ::htons(TcpPort);
    static_cast<void>(::inet_pton(AF_INET, serverIp, &address.sin_addr));

    // The peer's server may not be listening yet, so retry rather than fail the run.
    for (std::uint32_t attempt = 0; attempt < ConnectAttempts; ++attempt)
    {
        if (::connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0)
        {
            const int one = 1;
            static_cast<void>(::setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one));
            return socketFd;
        }
        ::usleep(ConnectRetryUs);
    }
    std::perror("connect");
    std::exit(1);
}

// Read or write exactly @bytes, since a short transfer here would desync the exchange.
[[nodiscard]] int transferAll(int sock, void* buffer, std::size_t bytes, bool isSend)
{
    std::size_t done = 0;
    char* cursor = static_cast<char*>(buffer);
    while (done < bytes)
    {
        const ssize_t moved = isSend ? ::write(sock, cursor + done, bytes - done)
                                     : ::read(sock, cursor + done, bytes - done);
        if (moved <= 0)
        {
            return -1;
        }
        done += static_cast<std::size_t>(moved);
    }
    return 0;
}

// Exchange QP identity and bring the pair up. @sendFirst orders the two halves: the
// client sends then receives, the server the other way round, so neither blocks forever.
[[nodiscard]] bool connectQueuePair(int sock, ibv_context* context, ibv_qp* queuePair, std::uint32_t psn,
                                    bool sendFirst)
{
    WireQp_t local;
    WireQp_t remote;
    union ibv_gid myGid;
    static_cast<void>(::ibv_query_gid(context, PortNum, static_cast<int>(g_gidIndex), &myGid));
    local.qpn = queuePair->qp_num;
    local.psn = psn;
    std::memcpy(local.gid, &myGid, GidBytes);

    const bool exchanged =
        sendFirst ? (transferAll(sock, &local, sizeof local, true) == 0 &&
                     transferAll(sock, &remote, sizeof remote, false) == 0)
                  : (transferAll(sock, &remote, sizeof remote, false) == 0 &&
                     transferAll(sock, &local, sizeof local, true) == 0);
    if (!exchanged)
    {
        std::fprintf(stderr, "queuePair exchange failed\n");
        return false;
    }
    return moveQpToRtr(queuePair, remote) == 0 && moveQpToRts(queuePair, local.psn) == 0;
}

// ── one-word remote operations ──────────────────────────────────────────

// The verbs state one client thread needs. atomicResult receives what a CAS or FAA
// fetched; dataWord carries an RDMA READ or WRITE payload. Both are registered once and
// reused, because registering per operation would dominate what is being measured.
struct Endpoint_t
{
    ibv_qp* queuePair{nullptr};
    ibv_cq* completionQueue{nullptr};
    std::uint64_t* atomicResult{nullptr};
    ibv_mr* atomicResultMr{nullptr};
    std::uint64_t* dataWord{nullptr};
    ibv_mr* dataWordMr{nullptr};
    WireMr_t remote{};
    RegionBases_t bases{};
};

// Post one signalled request and spin its completion off the CQ. Every operation here is
// a single WR, so polling beats an event: the wait is one fabric round trip.
void postAndWait(Endpoint_t& endpoint, ibv_send_wr& request, const char* what)
{
    ibv_send_wr* bad = nullptr;
    if (::ibv_post_send(endpoint.queuePair, &request, &bad) != 0)
    {
        std::perror(what);
        std::exit(1);
    }

    ibv_wc completion;
    int completed = 0;
    do
    {
        completed = ::ibv_poll_cq(endpoint.completionQueue, 1, &completion);
    } while (completed == 0);

    if (completion.status != IBV_WC_SUCCESS)
    {
        std::fprintf(stderr, "%s wc status=%s\n", what, ::ibv_wc_status_str(completion.status));
        std::exit(1);
    }
}

// Compare-and-swap on the lock word of @domain. Returns the value that was there, so the
// caller acquired the lock iff it reads back as free.
[[nodiscard]] std::uint64_t compareAndSwap(Endpoint_t& endpoint, std::uint32_t domain,
                                           std::uint64_t expected, std::uint64_t desired)
{
    ibv_sge segment = {reinterpret_cast<std::uintptr_t>(endpoint.atomicResult), WordBytes,
                       endpoint.atomicResultMr->lkey};
    ibv_send_wr request;
    std::memset(&request, 0, sizeof request);
    request.wr_id = 1;
    request.sg_list = &segment;
    request.num_sge = 1;
    request.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
    request.send_flags = IBV_SEND_SIGNALED;
    request.wr.atomic.remote_addr =
        endpoint.bases.lock + static_cast<std::uint64_t>(domain) * endpoint.remote.stride;
    request.wr.atomic.rkey = endpoint.remote.rkey;
    request.wr.atomic.compare_add = expected;
    request.wr.atomic.swap = desired;

    postAndWait(endpoint, request, "CAS");
    return *endpoint.atomicResult;
}

// NIC fetch-and-add of 1. Returns the value from before the add.
[[nodiscard]] std::uint64_t fetchAndAdd(Endpoint_t& endpoint, std::uint64_t remoteAddr)
{
    ibv_sge segment = {reinterpret_cast<std::uintptr_t>(endpoint.atomicResult), WordBytes,
                       endpoint.atomicResultMr->lkey};
    ibv_send_wr request;
    std::memset(&request, 0, sizeof request);
    request.wr_id = 3;
    request.sg_list = &segment;
    request.num_sge = 1;
    request.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
    request.send_flags = IBV_SEND_SIGNALED;
    request.wr.atomic.remote_addr = remoteAddr;
    request.wr.atomic.rkey = endpoint.remote.rkey;
    request.wr.atomic.compare_add = 1;

    postAndWait(endpoint, request, "FAA");
    return *endpoint.atomicResult;
}

// A plain, NON-atomic RDMA READ or WRITE of one word. IBV_WR_RDMA_READ fills dataWord;
// IBV_WR_RDMA_WRITE ships it. The software read-modify-write in --verify is READ, ++, WRITE,
// which is exactly the sequence a lock has to make safe.
void rdmaWord(Endpoint_t& endpoint, ibv_wr_opcode opcode, std::uint64_t remoteAddr,
              const char* what)
{
    ibv_sge segment = {reinterpret_cast<std::uintptr_t>(endpoint.dataWord), WordBytes,
                       endpoint.dataWordMr->lkey};
    ibv_send_wr request;
    std::memset(&request, 0, sizeof request);
    request.wr_id = 2;
    request.sg_list = &segment;
    request.num_sge = 1;
    request.opcode = opcode;
    request.send_flags = IBV_SEND_SIGNALED;
    request.wr.rdma.remote_addr = remoteAddr;
    request.wr.rdma.rkey = endpoint.remote.rkey;

    postAndWait(endpoint, request, what);
}

[[nodiscard]] std::uint64_t counterAddr(const Endpoint_t& endpoint, std::uint32_t domain)
{
    return endpoint.bases.counter + static_cast<std::uint64_t>(domain) * endpoint.remote.stride;
}

// Take the lock on @domain and return once it is held.
//
// --ticket: my = FAA(ticket[d]), then read serving[d] until it names us. FCFS, bounded by
// the number of peers ahead, and no retry storm, because nobody re-attempts the word.
// The fixed inter-poll spin keeps the read from hammering the NIC between turns.
//
// Otherwise: CAS the word from free to our id. On failure, either retry immediately
// (blind, quadratic in the worst case) or wait a jittered backoff first.
void acquire(Endpoint_t& endpoint, std::uint32_t domain, std::uint64_t myId, std::uint32_t& rng)
{
    const std::uint64_t stride = endpoint.remote.stride;

    if (g_useTicket)
    {
        const std::uint64_t myTicket =
            fetchAndAdd(endpoint, endpoint.bases.ticket + static_cast<std::uint64_t>(domain) * stride);
        for (;;)
        {
            rdmaWord(endpoint, IBV_WR_RDMA_READ,
                     endpoint.bases.serving + static_cast<std::uint64_t>(domain) * stride, "RREAD");
            if (*endpoint.dataWord == myTicket)
            {
                return;
            }
            spinUntil(readClockUs() + g_ticketSpinUs);
        }
    }

    double backoffUs = BackoffStartUs;
    for (;;)
    {
        if (compareAndSwap(endpoint, domain, 0, myId) == 0)
        {
            return;
        }
        if (g_useBackoff)
        {
            const double jitter =
                JitterFloor + static_cast<double>(nextRandom(rng) & JitterMask) / JitterDivisor;
            spinUntil(readClockUs() + backoffUs * jitter);
            backoffUs = std::min(backoffUs * 2.0, BackoffCapUs);
        }
    }
}

// Release the lock on @domain. The ticket path advances serving[d]; only the holder ever
// releases, so a plain add is safe there. The CAS path stores our id back to free.
void release(Endpoint_t& endpoint, std::uint32_t domain, std::uint64_t myId)
{
    if (g_useTicket)
    {
        static_cast<void>(fetchAndAdd(endpoint, endpoint.bases.serving +
                                                    static_cast<std::uint64_t>(domain) *
                                                        endpoint.remote.stride));
        return;
    }
    static_cast<void>(compareAndSwap(endpoint, domain, myId, 0));
}

// ── server ──────────────────────────────────────────────────────────────

void runServer(std::uint32_t qpCount, std::uint32_t domains)
{
    ibv_context* context = openDevice();
    ibv_pd* protectionDomain = ::ibv_alloc_pd(context);

    // One MR covering all four regions. The unused ones are never touched, and the
    // REMOTE_READ access is what --verify and --ticket need for their RDMA reads.
    const std::size_t bytes =
        static_cast<std::size_t>(RegionCount) * domains * DomainStrideBytes;
    void* buffer = nullptr;
    if (::posix_memalign(&buffer, PageBytes, bytes) != 0)
    {
        std::perror("memalign");
        std::exit(1);
    }
    std::memset(buffer, 0, bytes);  // locks unlocked, counters/tickets/serving all 0

    ibv_mr* memoryRegion = ::ibv_reg_mr(protectionDomain, buffer, bytes,
                                        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                                            IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
    if (memoryRegion == nullptr)
    {
        std::perror("reg_mr");
        std::exit(1);
    }

    const int listener = listenOnTcp();
    std::fprintf(stderr, "[server] domains=%u nqp=%u addr=%p rkey=0x%x listening\n", domains,
                 qpCount, buffer, memoryRegion->rkey);

    std::vector<int> clientFds(qpCount, -1);
    for (std::uint32_t index = 0; index < qpCount; ++index)
    {
        const int clientFd = ::accept(listener, nullptr, nullptr);
        if (clientFd < 0)
        {
            std::perror("accept");
            std::exit(1);
        }
        clientFds[index] = clientFd;

        ibv_cq* completionQueue = nullptr;
        ibv_qp* queuePair = createQueuePair(context, protectionDomain, &completionQueue);

        // The client sends its QP first, so this side receives first.
        WireQp_t local;
        WireQp_t remote;
        union ibv_gid myGid;
        static_cast<void>(::ibv_query_gid(context, PortNum, static_cast<int>(g_gidIndex), &myGid));
        local.qpn = queuePair->qp_num;
        local.psn = ServerPsnBase + index;
        std::memcpy(local.gid, &myGid, GidBytes);

        if (transferAll(clientFd, &remote, sizeof remote, false) != 0)
        {
            std::fprintf(stderr, "recv queuePair fail\n");
            std::exit(1);
        }
        if (transferAll(clientFd, &local, sizeof local, true) != 0)
        {
            std::fprintf(stderr, "send queuePair fail\n");
            std::exit(1);
        }

        WireMr_t wireMr = {static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buffer)),
                           memoryRegion->rkey, domains, static_cast<std::uint32_t>(DomainStrideBytes)};
        if (transferAll(clientFd, &wireMr, sizeof wireMr, true) != 0)
        {
            std::fprintf(stderr, "send memoryRegion fail\n");
            std::exit(1);
        }
        if (moveQpToRtr(queuePair, remote) != 0 || moveQpToRts(queuePair, local.psn) != 0)
        {
            std::exit(1);
        }
    }

    std::fprintf(stderr, "[server] all %u QPs connected; NIC serves atomics. waiting.\n", qpCount);

    // Each client writes one byte when it is finished. The server's CPU never enters the
    // lock path, which is the whole point of the comparison.
    for (std::uint32_t index = 0; index < qpCount; ++index)
    {
        char byte = 0;
        const ssize_t got = ::read(clientFds[index], &byte, 1);
        static_cast<void>(got);
    }
    std::fprintf(stderr, "[server] done.\n");
}

// ── client ──────────────────────────────────────────────────────────────

struct ThreadArgs_t
{
    std::uint32_t peerId{0};
    const char* serverIp{nullptr};
    std::uint32_t domains{0};
    std::uint32_t iterations{0};  // measured sweeps
    std::uint32_t warmup{0};      // unmeasured sweeps
    bool verify{false};
    ibv_pd* protectionDomain{nullptr};
    ibv_context* context{nullptr};
    pthread_barrier_t* startBarrier{nullptr};
    pthread_barrier_t* readbackBarrier{nullptr};  // verify: all RMWs done before readback
    std::vector<double> latenciesUs;              // perf mode, one per acquire
    std::vector<std::uint64_t>* counts{nullptr};  // verify: thread 0 reads every counter here
};

// Register one 8-byte local buffer for a remote operation to land in.
[[nodiscard]] std::uint64_t* registerWord(ibv_pd* protectionDomain, ibv_mr** mrOut,
                                          const char* what)
{
    void* raw = nullptr;
    if (::posix_memalign(&raw, WordBytes, WordBytes) != 0)
    {
        std::perror("memalign");
        std::exit(1);
    }
    ibv_mr* memoryRegion = ::ibv_reg_mr(protectionDomain, raw, WordBytes, IBV_ACCESS_LOCAL_WRITE);
    if (memoryRegion == nullptr)
    {
        std::perror(what);
        std::exit(1);
    }
    *mrOut = memoryRegion;
    return static_cast<std::uint64_t*>(raw);
}

// Visit order for one sweep, shuffled in place so two peers do not walk the domains in
// lockstep and turn a contention measurement into a convoy.
void shuffle(std::vector<std::uint32_t>& order, std::uint32_t& rng)
{
    for (std::size_t index = order.size(); index > 1; --index)
    {
        const std::size_t pick = nextRandom(rng) % index;
        std::swap(order[index - 1], order[pick]);
    }
}

// Brings up this thread's queue pair, takes the server's memory region off @sock, and registers
// the two local words every later operation reads and writes. Exits on failure: a client that
// cannot reach the server has nothing to measure.
[[nodiscard]] Endpoint_t openClientEndpoint(const ThreadArgs_t& args, int sock)
{
    Endpoint_t endpoint;
    endpoint.queuePair = createQueuePair(args.context, args.protectionDomain, &endpoint.completionQueue);
    if (!connectQueuePair(sock, args.context, endpoint.queuePair, ClientPsnBase + args.peerId, true))
    {
        std::exit(1);
    }

    WireMr_t wireMr;
    if (transferAll(sock, &wireMr, sizeof wireMr, false) != 0)
    {
        std::fprintf(stderr, "recv memoryRegion fail\n");
        std::exit(1);
    }
    endpoint.remote = wireMr;
    endpoint.bases = computeRegionBases(wireMr);
    endpoint.atomicResult = registerWord(args.protectionDomain, &endpoint.atomicResultMr, "reg local");
    endpoint.dataWord = registerWord(args.protectionDomain, &endpoint.dataWordMr, "reg cbuf");
    return endpoint;
}

// Correctness soak: `iterations` sweeps of acquire -> non-atomic RMW -> release, each visiting
// every domain, so a correct lock yields counter[d] == peers*iters. Peer 0 reads every counter
// back once all peers have stopped writing.
void runVerifySweeps(Endpoint_t& endpoint, ThreadArgs_t& args, std::uint64_t myId,
                     std::vector<std::uint32_t>& order, std::uint32_t& rng)
{
    for (std::uint32_t sweep = 0; sweep < args.iterations; ++sweep)
    {
        shuffle(order, rng);
        for (const std::uint32_t domain : order)
        {
            acquire(endpoint, domain, myId, rng);
            rdmaWord(endpoint, IBV_WR_RDMA_READ, counterAddr(endpoint, domain), "RDMA");
            *endpoint.dataWord += 1;
            rdmaWord(endpoint, IBV_WR_RDMA_WRITE, counterAddr(endpoint, domain), "RDMA");
            release(endpoint, domain, myId);
        }
    }

    ::pthread_barrier_wait(args.readbackBarrier);  // all peers done before readback
    if (args.peerId != 0)                          // one reader dumps every counter
    {
        return;
    }
    for (std::uint32_t domain = 0; domain < args.domains; ++domain)
    {
        rdmaWord(endpoint, IBV_WR_RDMA_READ, counterAddr(endpoint, domain), "RDMA");
        (*args.counts)[domain] = *endpoint.dataWord;
    }
}

// Performance mode: empty critical section, so what is timed is acquire alone. The warmup
// sweeps run the same path and record nothing.
void runTimedSweeps(Endpoint_t& endpoint, ThreadArgs_t& args, std::uint64_t myId,
                    std::vector<std::uint32_t>& order, std::uint32_t& rng)
{
    const std::uint32_t sweeps = args.warmup + args.iterations;
    args.latenciesUs.reserve(static_cast<std::size_t>(args.iterations) * args.domains);
    for (std::uint32_t sweep = 0; sweep < sweeps; ++sweep)
    {
        const bool measured = (sweep >= args.warmup);
        shuffle(order, rng);
        for (const std::uint32_t domain : order)
        {
            const double startUs = measured ? readClockUs() : 0.0;
            acquire(endpoint, domain, myId, rng);
            const double heldUs = measured ? readClockUs() : 0.0;
            release(endpoint, domain, myId);
            if (measured)
            {
                args.latenciesUs.push_back(heldUs - startUs);
            }
        }
    }
}

void* clientThread(void* arg)
{
    auto* args = static_cast<ThreadArgs_t*>(arg);
    const std::uint64_t myId = static_cast<std::uint64_t>(args->peerId) + 1;  // nonzero holder id

    const std::int32_t fileDesc = connectOverTcp(args->serverIp);
    Endpoint_t endpoint = openClientEndpoint(*args, fileDesc);

    std::vector<std::uint32_t> order(args->domains);
    for (std::uint32_t domain = 0; domain < args->domains; ++domain)
    {
        order[domain] = domain;
    }
    std::uint32_t rng = (args->peerId + 1) * SeedMultiplier;  // per-thread seed

    ::pthread_barrier_wait(args->startBarrier);

    if (args->verify)
    {
        runVerifySweeps(endpoint, *args, myId, order, rng);
    }
    else
    {
        runTimedSweeps(endpoint, *args, myId, order, rng);
    }

    const char done = 1;
    const ssize_t written = ::write(fileDesc, &done, 1);
    static_cast<void>(written);
    return nullptr;
}

// Start one thread per peer on this host and wait for all of them.
void runPeers(std::vector<ThreadArgs_t>& args)
{
    std::vector<pthread_t> threads(args.size());
    for (std::size_t index = 0; index < args.size(); ++index)
    {
        ::pthread_create(&threads[index], nullptr, clientThread, &args[index]);
    }
    for (const pthread_t& thread : threads)
    {
        ::pthread_join(thread, nullptr);
    }
}

// Correctness soak. Returns 0 when no domain lost an update, 1 otherwise.
[[nodiscard]] int runVerify(const char* serverIp, std::uint32_t peers, std::uint32_t domains,
                            std::uint32_t iterations)
{
    ibv_context* context = openDevice();
    ibv_pd* protectionDomain = ::ibv_alloc_pd(context);

    pthread_barrier_t startBarrier;
    pthread_barrier_t readbackBarrier;
    ::pthread_barrier_init(&startBarrier, nullptr, peers);
    ::pthread_barrier_init(&readbackBarrier, nullptr, peers);

    std::vector<std::uint64_t> counts(domains, 0);
    std::vector<ThreadArgs_t> args(peers);
    for (std::uint32_t index = 0; index < peers; ++index)
    {
        args[index].peerId = index;
        args[index].serverIp = serverIp;
        args[index].domains = domains;
        args[index].iterations = iterations;
        args[index].verify = true;
        args[index].protectionDomain = protectionDomain;
        args[index].context = context;
        args[index].startBarrier = &startBarrier;
        args[index].readbackBarrier = &readbackBarrier;
        args[index].counts = &counts;
    }
    runPeers(args);

    const std::uint64_t expected = static_cast<std::uint64_t>(peers) * iterations;
    std::uint32_t failures = 0;
    std::printf("VERIFY soak: peers=%u domains=%u iters=%u  (expected counter = %" PRIu64
                " per domain)\n",
                peers, domains, iterations, expected);
    for (std::uint32_t domain = 0; domain < domains; ++domain)
    {
        const std::uint64_t got = counts[domain];
        if (got != expected)
        {
            ++failures;
            std::printf("  domain %-3u expected %" PRIu64 " got %" PRIu64 "  (%" PRIu64
                        " LOST updates)\n",
                        domain, expected, got, expected - got);
        }
        else if (domains <= VerifyEchoDomainLimit)
        {
            std::printf("  domain %-3u expected %" PRIu64 " got %" PRIu64 "  OK\n", domain, expected,
                        got);
        }
    }

    if (failures == 0)
    {
        std::printf("VERIFY: PASS\n");
        return 0;
    }
    std::printf("VERIFY: FAIL (%u/%u domains had lost updates)\n", failures, domains);
    return 1;
}

struct Summary_t
{
    double meanUs{0.0};
    double p50Us{0.0};
    double p90Us{0.0};
    double p99Us{0.0};
    double throughputOps{0.0};
    std::uint64_t samples{0};
};

[[nodiscard]] double percentileOf(const std::vector<double>& sorted, double fraction)
{
    if (sorted.empty())
    {
        return 0.0;
    }
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(sorted.size()));
    return sorted[std::min(index, sorted.size() - 1)];
}

[[nodiscard]] Summary_t runClient(const char* serverIp, std::uint32_t peers, std::uint32_t domains,
                                  std::uint32_t iterations, std::uint32_t warmup)
{
    ibv_context* context = openDevice();
    ibv_pd* protectionDomain = ::ibv_alloc_pd(context);

    pthread_barrier_t startBarrier;
    ::pthread_barrier_init(&startBarrier, nullptr, peers);

    std::vector<ThreadArgs_t> args(peers);
    for (std::uint32_t index = 0; index < peers; ++index)
    {
        args[index].peerId = index;
        args[index].serverIp = serverIp;
        args[index].domains = domains;
        args[index].iterations = iterations;
        args[index].warmup = warmup;
        args[index].protectionDomain = protectionDomain;
        args[index].context = context;
        args[index].startBarrier = &startBarrier;
    }

    const double startUs = readClockUs();
    runPeers(args);
    const double wallUs = readClockUs() - startUs;

    // Throughput is over the wall clock of the whole run, so it counts the contention the
    // per-acquire latencies were paid under.
    std::vector<double> all;
    for (const ThreadArgs_t& one : args)
    {
        all.insert(all.end(), one.latenciesUs.begin(), one.latenciesUs.end());
    }
    if (all.empty())
    {
        return Summary_t{};
    }

    double sum = 0.0;
    for (const double sample : all)
    {
        sum += sample;
    }
    std::sort(all.begin(), all.end());

    Summary_t summary;
    summary.samples = all.size();
    summary.meanUs = sum / static_cast<double>(all.size());
    summary.p50Us = percentileOf(all, MedianFraction);
    summary.p90Us = percentileOf(all, P90Fraction);
    summary.p99Us = percentileOf(all, P99Fraction);
    summary.throughputOps = static_cast<double>(all.size()) / (wallUs / UsPerSecond);
    return summary;
}

constexpr const char* Usage =
    "usage: %s --server --qps N --domains D |\n"
    "       %s --client <ip> --peers N --domains D [--iters I --warmup W]\n"
    "          [--backoff|--blind|--ticket] [--verify] [--dev NAME] [--gid INDEX]\n"
    "  acquire policy precedence: --ticket > --blind/--backoff (default --backoff)\n";

// Exits rather than returning a default: a mistyped count would otherwise be measured and
// reported as if it were the count that was asked for.
[[nodiscard]] std::uint64_t parseU64(const char* text, const char* flag)
{
    char* end = nullptr;
    const std::uint64_t value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0')
    {
        std::fprintf(stderr, "%s wants a number, got '%s'\n", flag, text);
        std::exit(2);
    }
    return static_cast<std::uint64_t>(value);
}

// What the command line asked for. The flags that select a retry policy or name a device land
// in the g_ globals instead, because the code paths that read them are not reached from here.
struct Args_t
{
    bool isServer{false};
    bool isClient{false};
    bool verify{false};
    const char* serverIp{nullptr};
    std::uint32_t peers{DefaultPeers};
    std::uint32_t domains{DefaultDomains};
    std::uint32_t iterations{DefaultIterations};
    std::uint32_t warmup{DefaultWarmup};
    std::uint32_t qpCount{DefaultQps};
};

// False on an unknown flag, which the caller turns into a usage exit. Split from main so the
// chain of comparisons is not counted against the dispatch that follows it.
[[nodiscard]] bool parseArgs(int argc, char** argv, Args_t& args)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string arg = argv[index];
        const bool hasValue = (index + 1 < argc);
        if (arg == "--server")
        {
            args.isServer = true;
        }
        else if (arg == "--verify")
        {
            args.verify = true;
        }
        else if (arg == "--backoff")
        {
            g_useBackoff = true;
        }
        else if (arg == "--blind")
        {
            g_useBackoff = false;
        }
        else if (arg == "--ticket")
        {
            g_useTicket = true;
        }
        else if (arg == "--client" && hasValue)
        {
            args.isClient = true;
            args.serverIp = argv[++index];
        }
        else if (arg == "--dev" && hasValue)
        {
            g_device = argv[++index];
        }
        else if (arg == "--peers" && hasValue)
        {
            args.peers = static_cast<std::uint32_t>(parseU64(argv[++index], "--peers"));
        }
        else if (arg == "--qps" && hasValue)
        {
            args.qpCount = static_cast<std::uint32_t>(parseU64(argv[++index], "--qps"));
        }
        else if (arg == "--domains" && hasValue)
        {
            args.domains = static_cast<std::uint32_t>(parseU64(argv[++index], "--domains"));
        }
        else if (arg == "--iters" && hasValue)
        {
            args.iterations = static_cast<std::uint32_t>(parseU64(argv[++index], "--iters"));
        }
        else if (arg == "--warmup" && hasValue)
        {
            args.warmup = static_cast<std::uint32_t>(parseU64(argv[++index], "--warmup"));
        }
        else if (arg == "--gid" && hasValue)
        {
            g_gidIndex = static_cast<std::uint8_t>(parseU64(argv[++index], "--gid"));
        }
        else
        {
            std::fprintf(stderr, "unknown arg %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

// The measured run, once the command line has been read and checked.
[[nodiscard]] int runAsClient(const Args_t& args)
{
    if (args.verify)
    {
        return runVerify(args.serverIp, args.peers, args.domains, args.iterations);
    }
    const Summary_t summary =
        runClient(args.serverIp, args.peers, args.domains, args.iterations, args.warmup);
    std::printf(
        "RESULT peers=%u domains=%u mean_us=%.3f p50_us=%.3f p90_us=%.3f p99_us=%.3f "
        "tput_ops=%.0f samples=%" PRIu64 "\n",
        args.peers, args.domains, summary.meanUs, summary.p50Us, summary.p90Us, summary.p99Us,
        summary.throughputOps, summary.samples);
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Args_t args;
    if (!parseArgs(argc, argv, args))
    {
        return 1;
    }
    if (args.isServer)
    {
        runServer(args.qpCount, args.domains);
        return 0;
    }
    if (!args.isClient)
    {
        std::fprintf(stderr, Usage, argv[0], argv[0]);
        return 1;
    }
    if (args.peers == 0 || args.domains == 0)
    {
        std::fprintf(stderr, "--peers and --domains must each be at least 1\n");
        return 2;
    }
    return runAsClient(args);
}
