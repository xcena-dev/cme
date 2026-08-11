// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// cme_top.cpp -- htop-style read-only monitor for a cme region.
//
// Built on cme::Inspector: never writes, never joins, never locks. Frame-to-frame deltas
// come from a small struct retained across iterations.
//
// Flags:
//   --once             one render then exit (scripting-friendly)
//   --interval=MS      refresh period in milliseconds (default 500)
//   --no-clear         do not ANSI-clear between frames (append mode)

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "config.hpp"
#include "config_reader.hpp"
#include "core/domain_bitmap.hpp"
#include "core/types.hpp"
#include "observe/inspector.hpp"

namespace
{

// Clear to end of line, then newline. Every frame line ends with this, so a line that got
// shorter does not leave the tail of the previous frame behind.
constexpr const char* ClearToLineEnd = "\x1b[K\n";
constexpr const char* CursorHome = "\x1b[H";
constexpr const char* ClearToScreenEnd = "\x1b[J";
constexpr const char* EnterAltScreen = "\x1b[?1049h\x1b[?25l\x1b[H\x1b[2J";
constexpr const char* LeaveAltScreen = "\x1b[?25h\x1b[?1049l";

constexpr std::uint32_t MaxPeers = 64;
constexpr std::uint32_t MaxDomains = 64;

constexpr std::uint32_t DefaultIntervalMs = 500;

// Floor on the refresh period. Below this the render itself starts to cost more than the
// interval, and the monitor competes with what it is watching.
constexpr std::uint32_t MinIntervalMs = 50;

// How often the sleep loop rechecks the stop flag, so Ctrl-C lands well inside one frame.
constexpr std::uint32_t StopCheckMs = 50;

constexpr std::size_t TimeTextBytes = 32;

constexpr double PercentScale = 100.0;

// A percentage past this is a broken counter rather than a load, so clamp and keep the
// column readable instead of printing six digits.
constexpr double MaxReportedPercent = 999.0;

constexpr std::string_view IntervalFlag = "--interval=";

constexpr const char* Usage =
    "usage: cme-top [--once] [--interval=MS] [--no-clear] [URI]\n"
    "       URI = dax:/dev/daxN.M | shm:/name | file:/path\n"
    "       omit it to use dax_device from config.yaml\n";

// The one unavoidable global: a signal handler may write nothing else.
volatile std::sig_atomic_t g_stopRequested = 0;

void onSignal(int)
{
    g_stopRequested = 1;
}

// Holds the alternate screen for as long as it is in scope. A monitor that exits without
// restoring the terminal leaves the caller's shell without a cursor, and the exit paths
// that matter here (the loop ending, a caught signal) all unwind through this scope.
class AltScreen
{
public:
    AltScreen()
    {
        std::fputs(EnterAltScreen, stdout);
        std::fflush(stdout);
    }

    AltScreen(const AltScreen&) = delete;
    AltScreen(AltScreen&&) = delete;
    AltScreen& operator=(const AltScreen&) = delete;
    AltScreen& operator=(AltScreen&&) = delete;

    ~AltScreen()
    {
        std::fputs(LeaveAltScreen, stdout);
        std::fflush(stdout);
    }
};

// Frame-to-frame deltas need a clock that cannot step, which is timing::monotonic. Peer ages need
// the one the heartbeat is stamped with, which is below. They are different clocks.

[[nodiscard]] const char* getStrategyName(cme::Strategy strategy)
{
    switch (strategy)
    {
        case cme::Strategy::Order:
            return "ORDER";
        case cme::Strategy::Request:
            return "REQUEST";
        case cme::Strategy::RequestAgg:
            return "REQ-AGG";
        case cme::Strategy::Peterson:
            return "PETERSON";
    }
    return "?";
}

// What a peer looks like from outside. Holding outranks Waiting: a peer can hold one
// domain while queued for another, and the hold is what the other peers are blocked on.
enum class Role
{
    None,
    Stale,
    Holding,
    Waiting,
    Idle,
};

[[nodiscard]] const char* getRoleName(Role role)
{
    switch (role)
    {
        case Role::None:
            return "NONE";
        case Role::Stale:
            return "STALE";
        case Role::Holding:
            return "HOLDING";
        case Role::Waiting:
            return "WAITING";
        case Role::Idle:
            return "IDLE";
    }
    return "?";
}

// Cells are built as strings rather than into fixed buffers, because a monitor refreshing
// twice a second does not care about the allocation and a buffer here would need its own
// capacity arithmetic to stay safe.
[[nodiscard]] std::string formatMask(const cme::DomainBitmap& mask)
{
    if (mask.isEmpty())
    {
        return "-";
    }
    const cme::DomainId limit = cme::DomainWordCount * cme::DomainBitsPerWord;
    std::string text = "{";
    bool isFirst = true;
    for (cme::DomainId domainId = 0; domainId < limit; ++domainId)
    {
        if (!mask.has(domainId))
        {
            continue;
        }
        if (!isFirst)
        {
            text += ",";
        }
        text += "d" + std::to_string(domainId);
        isFirst = false;
    }
    text += "}";
    return text;
}

// A CPU-time counter as a share of the frame. "-" when the frame has no width yet, or when
// the counter went backwards, which means the peer restarted rather than that it idled.
[[nodiscard]] std::string formatCpuShare(std::uint64_t current, std::uint64_t previous,
                                         std::uint64_t frameNs)
{
    if (frameNs == 0 || current == 0 || current < previous)
    {
        return "-";
    }
    const double share =
        static_cast<double>(current - previous) * PercentScale / static_cast<double>(frameNs);
    char text[16]{};
    std::snprintf(text, sizeof text, "%.1f%%", std::min(share, MaxReportedPercent));
    return text;
}

// Busy-spin share of the time spent waiting, which separates "blocked" from "burning a core
// while blocked".
[[nodiscard]] std::string formatSpinShare(std::uint64_t waitNs, std::uint64_t prevWaitNs,
                                          std::uint64_t spinNs, std::uint64_t prevSpinNs)
{
    if (waitNs <= prevWaitNs || spinNs < prevSpinNs)
    {
        return "-";
    }
    const double share = static_cast<double>(spinNs - prevSpinNs) * PercentScale /
                         static_cast<double>(waitNs - prevWaitNs);
    char text[16]{};
    std::snprintf(text, sizeof text, "%.1f%%", std::min(share, PercentScale));
    return text;
}

// A stamp older than this is stale. It is the region's own failure-detector window, skew
// bound and cache staleness included, so STALE here means "the other peers would call this
// one dead" rather than "it did not tick during whatever refresh the operator picked".
// A peer that has never stamped, or whose stamp sits ahead of this host's clock, has no age
// to judge. Neither is evidence of death, so neither reads as stale.
[[nodiscard]] bool isHeartbeatStale(timing::WallStamp lastSeen, std::uint64_t nowNs)
{
    if (lastSeen.isUnset())
    {
        return false;
    }
    const std::optional<timing::Nanos> age = lastSeen.ageAt(nowNs);
    return age.has_value() && *age > cme::DeadWindowEffective;
}

// How far ahead of this host's clock a stamp may sit before it is worth naming. The frame
// reads its clock once and then walks the peer records, so a peer stamping during that walk
// lands microseconds in the future through sampling order alone. Past the bound the protocol
// declares for pairwise skew, it is the clocks that disagree.
// Time since the peer last stamped its liveness witness.
[[nodiscard]] std::string formatHeartbeatAge(timing::WallStamp lastSeen, std::uint64_t nowNs)
{
    if (lastSeen.isUnset())
    {
        return "-";
    }
    if (const std::optional<timing::Nanos> ahead = lastSeen.aheadAt(nowNs))
    {
        return (*ahead > cme::ClockSkewBound) ? "skew" : "0ms";
    }

    // Neither ahead nor behind: the stamp landed on this frame's own instant.
    const std::optional<timing::Nanos> age = lastSeen.ageAt(nowNs);
    if (!age)
    {
        return "0ms";
    }

    char text[16]{};
    std::snprintf(text, sizeof text, "%.0fms", timing::MillisF{*age}.count());
    return text;
}

// The holder, and what it was on the previous frame when that differs, so a handoff is
// visible in the frame it happens rather than only as a changed number.
//
// The arrow is ASCII because printf pads a column by bytes: a multi-byte arrow makes the
// holder column two positions narrow and every later column on that row shifts left.
[[nodiscard]] std::string formatHolder(cme::PeerId current, cme::PeerId previous, bool hasPrevious)
{
    std::string text = (current == cme::NoPeer) ? "-" : std::to_string(current);
    if (hasPrevious && previous != current && previous != cme::NoPeer)
    {
        text += "<-" + std::to_string(previous);
    }
    return text;
}

struct Opts_t
{
    std::string path;
    std::uint32_t intervalMs{DefaultIntervalMs};
    bool once{false};
    bool noClear{false};
};

struct FrameState_t
{
    bool isFirstFrame{true};
    std::uint64_t steadyNs{0};
    std::uint64_t prevPollNs[MaxPeers]{};
    std::uint64_t prevWorkerNs[MaxPeers]{};
    std::uint64_t prevWaitNs[MaxPeers]{};
    std::uint64_t prevSpinNs[MaxPeers]{};
    cme::PeerId prevHolder[MaxDomains];

    FrameState_t() noexcept
    {
        std::fill(std::begin(prevHolder), std::end(prevHolder), cme::NoPeer);
    }
};

using OwnershipList = std::vector<std::optional<cme::Inspector::OwnershipSnapshot_t>>;

void renderSummary(const cme::Inspector::HeaderSnapshot_t& info, const Opts_t& options,
                   const char* timeText)
{
    std::printf("cme-top  %s  [%s]%s", options.path.c_str(), timeText, ClearToLineEnd);
    std::printf("  strategy=%-8s  domains=%u  peers=%u  total=%" PRIu64 "B  fmt_gen=%" PRIu64
                "%s%s",
                getStrategyName(info.strategy), info.numDomains, info.maxPeers, info.totalSize,
                info.formatGeneration, ClearToLineEnd, ClearToLineEnd);
}

// Domain 0 is the control domain by definition (core/types.hpp), and format() leaves its
// name empty because nobody creates it. Every other unnamed domain is a spare slot.
[[nodiscard]] std::string formatDomainName(cme::DomainId domainId, const std::string& stored)
{
    if (!stored.empty())
    {
        return stored;
    }
    return cme::isControlDomain(domainId) ? "control" : "-";
}

// Returns which peers hold at least one domain, which is what decides the Holding role.
[[nodiscard]] std::vector<bool> renderDomains(const OwnershipList& ownership, FrameState_t& state)
{
    std::vector<bool> holdsAnyDomain(MaxPeers, false);

    std::printf("DOMAINS%s", ClearToLineEnd);
    std::printf("  %3s  %-12s  %-9s  %-14s%s", "id", "name", "holder", "generation",
                ClearToLineEnd);
    for (std::uint32_t domainId = 0; domainId < ownership.size(); ++domainId)
    {
        // Bound once: each ownership[domainId] is a fresh call, so the has_value() above says
        // nothing about a later subscript as far as a reader or a checker can tell.
        const auto& entry = ownership[domainId];
        if (!entry.has_value())
        {
            std::printf("  %3u  BAD%s", domainId, ClearToLineEnd);
            continue;
        }
        const cme::PeerId holder = entry->holder;
        if (holder != cme::NoPeer && holder < MaxPeers)
        {
            holdsAnyDomain[holder] = true;
        }
        const std::string holderText =
            formatHolder(holder, state.prevHolder[domainId], !state.isFirstFrame);
        std::printf("  %3u  %-12s  %-9s  %-14" PRIu64 "%s", domainId,
                    formatDomainName(domainId, entry->name).c_str(), holderText.c_str(),
                    entry->epoch, ClearToLineEnd);
        state.prevHolder[domainId] = holder;
    }
    std::printf("%s", ClearToLineEnd);
    return holdsAnyDomain;
}

[[nodiscard]] Role classifyPeer(const cme::Inspector::PeerSnapshot_t& peer, bool isStale,
                                bool holdsAnyDomain)
{
    if (!peer.active)
    {
        return Role::None;
    }
    if (isStale)
    {
        return Role::Stale;
    }
    if (holdsAnyDomain)
    {
        return Role::Holding;
    }
    if (!peer.pendingDomains.isEmpty())
    {
        return Role::Waiting;
    }
    return Role::Idle;
}

void renderPeers(cme::Inspector& inspector, std::uint32_t peerCount,
                 const std::vector<bool>& holdsAnyDomain, FrameState_t& state,
                 std::uint64_t frameNs)
{
    const std::uint64_t realtimeNs = timing::wall<timing::Nanos>();

    std::printf("PEERS%s", ClearToLineEnd);
    std::printf("  %3s  %-6s  %-8s  %-7s  %-9s  %-7s  %-7s  %-9s  %s%s", "id", "status", "role",
                "poll%", "worker%", "wait%", "spin%", "hb_age", "pending", ClearToLineEnd);

    for (std::uint32_t peerId = 0; peerId < peerCount; ++peerId)
    {
        const std::optional<cme::Inspector::PeerSnapshot_t> peer = inspector.readPeer(peerId);
        if (!peer.has_value())
        {
            std::printf("  %3u  BAD%s", peerId, ClearToLineEnd);
            continue;
        }

        const bool isStale = isHeartbeatStale(timing::WallStamp{peer->lastSeenNanos}, realtimeNs);
        const Role role = classifyPeer(*peer, isStale, holdsAnyDomain[peerId]);

        const std::uint64_t width = state.isFirstFrame ? 0 : frameNs;
        const std::string pollText = formatCpuShare(peer->time.poll, state.prevPollNs[peerId], width);
        const std::string workerText =
            formatCpuShare(peer->time.worker, state.prevWorkerNs[peerId], width);
        const std::string waitText = formatCpuShare(peer->time.wait, state.prevWaitNs[peerId], width);
        const std::string spinText =
            state.isFirstFrame ? "-"
                               : formatSpinShare(peer->time.wait, state.prevWaitNs[peerId],
                                                 peer->time.spin, state.prevSpinNs[peerId]);

        state.prevPollNs[peerId] = peer->time.poll;
        state.prevWorkerNs[peerId] = peer->time.worker;
        state.prevWaitNs[peerId] = peer->time.wait;
        state.prevSpinNs[peerId] = peer->time.spin;

        std::printf("  %3u  %-6s  %-8s  %-7s  %-9s  %-7s  %-7s  %-9s  %s%s", peerId,
                    peer->active ? "ACTIVE" : "NONE", getRoleName(role), pollText.c_str(),
                    workerText.c_str(), waitText.c_str(), spinText.c_str(),
                    formatHeartbeatAge(timing::WallStamp{peer->lastSeenNanos}, realtimeNs).c_str(),
                    formatMask(peer->pendingDomains).c_str(), ClearToLineEnd);
    }
    std::printf("%s", ClearToLineEnd);
}

void renderUnformatted(const Opts_t& options, const char* timeText, FrameState_t& state)
{
    std::printf("cme-top  %s  [%s]%s", options.path.c_str(), timeText, ClearToLineEnd);
    std::printf("  invalid CME state -- region not formatted or header magic mismatch%s",
                ClearToLineEnd);
    std::printf("  waiting for format() to land...%s", ClearToLineEnd);
    if (!options.once)
    {
        std::printf("%s[Ctrl-C to quit, refresh %ums]%s", ClearToLineEnd, options.intervalMs,
                    ClearToLineEnd);
    }
    if (!options.noClear)
    {
        std::fputs(ClearToScreenEnd, stdout);
    }
    std::fflush(stdout);
    state.isFirstFrame = true;
}

void render(cme::Inspector& inspector, const Opts_t& options, FrameState_t& state)
{
    if (!options.noClear)
    {
        std::fputs(CursorHome, stdout);
    }

    const std::time_t wallSeconds = std::time(nullptr);
    struct tm localTime = {};
    ::localtime_r(&wallSeconds, &localTime);
    char timeText[TimeTextBytes]{};
    std::strftime(timeText, sizeof timeText, "%H:%M:%S", &localTime);

    const std::optional<cme::Inspector::HeaderSnapshot_t> info = inspector.readHeader();
    if (!info.has_value())
    {
        renderUnformatted(options, timeText, state);
        return;
    }

    const std::uint64_t steadyNs = timing::monotonic<timing::Nanos>();
    const std::uint64_t frameNs = state.isFirstFrame ? 0 : (steadyNs - state.steadyNs);
    const std::uint32_t domainCount = std::min<std::uint32_t>(info->numDomains, MaxDomains);
    const std::uint32_t peerCount = std::min<std::uint32_t>(info->maxPeers, MaxPeers);

    // Sample every ownership record before rendering, so one frame shows one instant rather
    // than a walk across several.
    OwnershipList ownership(domainCount);
    for (std::uint32_t domainId = 0; domainId < domainCount; ++domainId)
    {
        ownership[domainId] = inspector.readOwnership(domainId);
    }

    renderSummary(*info, options, timeText);
    const std::vector<bool> holdsAnyDomain = renderDomains(ownership, state);
    renderPeers(inspector, peerCount, holdsAnyDomain, state, frameNs);

    if (!options.once)
    {
        std::printf("[Ctrl-C to quit, refresh %ums]%s", options.intervalMs, ClearToLineEnd);
    }
    if (!options.noClear)
    {
        std::fputs(ClearToScreenEnd, stdout);
    }
    std::fflush(stdout);

    state.steadyNs = steadyNs;
    state.isFirstFrame = false;
}

// Exits rather than defaulting: a mistyped interval would otherwise be silently clamped to
// the floor and the run would look like it honoured what was asked for.
[[nodiscard]] std::uint32_t parseIntervalMs(std::string_view text)
{
    const std::string value{text};
    char* end = nullptr;
    const std::uint64_t parsed = std::strtoull(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end != '\0')
    {
        std::fprintf(stderr, "%s wants a number of milliseconds, got '%s'\n",
                     std::string{IntervalFlag}.c_str(), value.c_str());
        std::exit(2);
    }
    // Narrowing a value this large would wrap, and wrapping past zero lands back on the
    // floor below, which would run at 50 ms while claiming to honour what was asked.
    if (parsed > std::numeric_limits<std::uint32_t>::max())
    {
        std::fprintf(stderr, "%s is out of range: %s\n", std::string{IntervalFlag}.c_str(),
                     value.c_str());
        std::exit(2);
    }
    return std::max(static_cast<std::uint32_t>(parsed), MinIntervalMs);
}

[[nodiscard]] bool parseArgs(int argc, char** argv, Opts_t& options)
{
    // Default target from config.yaml, so this machine's path is not compiled in. A
    // positional URI on the command line still wins. Empty when the machine has no devdax
    // node, or has one the config names but the node is not a character device.
    const harness::ConfigReader config;
    if (!config.site().daxDevice.empty())
    {
        options.path = config.site().daxDevice;
    }

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view arg = argv[index];
        if (arg == "--once")
        {
            options.once = true;
        }
        else if (arg == "--no-clear")
        {
            options.noClear = true;
        }
        else if (arg.rfind(IntervalFlag, 0) == 0)
        {
            options.intervalMs = parseIntervalMs(arg.substr(IntervalFlag.size()));
        }
        else if (!arg.empty() && arg.front() != '-')
        {
            options.path = std::string{arg};
        }
        else
        {
            std::fprintf(stderr, "unknown arg: %s\n%s", argv[index], Usage);
            return false;
        }
    }

    if (options.path.empty())
    {
        std::fputs(Usage, stderr);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    Opts_t options;
    if (!parseArgs(argc, argv, options))
    {
        return 2;
    }

    // A region that is not there, or not a region at all, is the ordinary way to mistype a
    // URI. Say which and why rather than letting the exception reach terminate().
    std::optional<cme::Inspector> inspector;
    try
    {
        inspector = cme::Inspector::open(options.path, cme::CoherencyMode::CacheCoherent);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "cannot open %s: %s\n", options.path.c_str(), error.what());
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    FrameState_t state;
    if (options.once)
    {
        render(*inspector, options, state);
        return 0;
    }

    const bool useTui = !options.noClear && (::isatty(STDOUT_FILENO) != 0);
    std::optional<AltScreen> altScreen;
    if (useTui)
    {
        altScreen.emplace();
    }

    while (g_stopRequested == 0)
    {
        render(*inspector, options, state);
        // The gap between frames, rechecked often enough that Ctrl-C lands well inside one.
        const timing::Deadline nextFrame{timing::Millis{options.intervalMs}};
        while (g_stopRequested == 0 && !nextFrame.expired())
        {
            std::this_thread::sleep_for(timing::Millis{StopCheckMs});
        }
    }

    if (!useTui)
    {
        std::fputc('\n', stdout);
    }
    return 0;
}
