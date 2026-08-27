// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// geometry.cpp -- Geometry factories, bind state, and format() writer.

#include "core/layout/geometry.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "cme/errors.hpp"
#include "cme/shared.hpp"
#include "common/poll.hpp"
#include "common/timing.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry_profile.hpp"
#include "core/policy/recovery_authority.hpp"
#include "core/policy/successor.hpp"
#include "core/types.hpp"
#include "memory/memory.hpp"
#include "observe/failpoint.hpp"
#include "util/coherency.hpp"
#include "util/endian.hpp"
#include "util/util.hpp"

namespace cme
{

namespace
{

// format() is the one write no joiner can re-read its way out of: it runs once, before any
// peer exists, and a joiner that misses it sees an unformatted region forever. So it uses the
// strictest regime rather than taking one as an option -- Flush is a superset of the other
// two, and picking Uncached for a WB-noncoherent formatter would leave the whole region in
// that peer's cache. The cost is bounded and paid once: at the 64x64 ceiling the region is
// ~1.03 MB, so ~16k lines at the measured 94-320 ns marginal flush, i.e. 1.5-5 ms against a
// 5 s format timeout.
inline constexpr CoherencyMode FormatCoherency = CoherencyMode::Flush;

// The gap between looks while waiting for a formatter to stamp the header. What is waited on is
// another process finishing a one-time write, so the look is cheap and the wait can be coarse.
inline constexpr timing::Millis HeaderPollGap{10};

[[nodiscard]] bool isHeaderReady(void* base, CoherencyMode mode) noexcept
{
    if (base == nullptr)
    {
        return false;
    }
    return coherency::get(static_cast<Geometry::Header_t*>(base), mode).isFormatted();  // rmb + 64B read
}

}  // namespace

// ── factories ──────────────────────────────────────────────────────

Geometry Geometry::open(std::string_view uri)
{
    return Geometry{Memory::open(uri)};
}

Geometry Geometry::create(std::string_view uri, std::uint32_t domainCount,
                          std::uint32_t peerCount, const FormatOpts_t& opts)
{
    if (!isValidDomainCount(domainCount) || !isValidPeerCount(peerCount))
    {
        throw FormatError{"cme::Geometry::create: invalid dims"};
    }
    // 0 means auto. Above peerCount the extra groups are empty -- peer p maps to p % G -- so
    // they only cost region bytes and a read on the holder's grant scan. Bounding it here is
    // what keeps the worst-case area a function of MaxDomains/MaxPeers alone.
    if (opts.aggregatorGroups > peerCount)
    {
        throw FormatError{"cme::Geometry::create: aggregatorGroups exceeds peerCount"};
    }
    const std::uint64_t areaSize =
        computeAreaSize(domainCount, peerCount, opts.strategy, opts.aggregatorGroups);
    auto memory = Memory::create(uri, areaSize);
    if (memory->getMappedSize() < areaSize)
    {
        throw FormatError{"cme::Geometry::create: backend mapping smaller than area"};
    }
    Geometry geometry{std::move(memory), domainCount, peerCount, opts.strategy, opts.aggregatorGroups};
    geometry.format(opts);
    return geometry;
}

// ── ops ────────────────────────────────────────────────────────────

void Geometry::format(const FormatOpts_t& opts)
{
    if (!bound_)
    {
        throw FormatError{"cme::Geometry::format: geometry not bound"};
    }
    // Re-checked here, not only in create(): this is public and the value lands in the header
    // for every later joiner to size its layout from.
    if (opts.aggregatorGroups > peerCount_)
    {
        throw FormatError{"cme::Geometry::format: aggregatorGroups exceeds peerCount"};
    }
    auto* base = static_cast<std::uint8_t*>(memory_->getBase());
    if (base == nullptr)
    {
        throw FormatError{"cme::Geometry::format: null base"};
    }

    const std::uint32_t domainCount = domainCount_;
    const std::uint32_t peerCount = peerCount_;
    const std::uint64_t totalSize = areaSize_;
    const Geometry& geometry = *this;

    // memset zeros State::Free / Status::None / holder=0 / epoch=0; loops only stamp magic and non-zero defaults.
    std::memset(base, 0, totalSize);

    // Slot 0 = control domain (Active, genesis holder=peer 0, not user-addressable).
    // Data slots 1..domainCount-1 stay Free; createDomain claims them at runtime.
    const std::uint32_t groupCount = getGroupCount(peerCount);
    for (std::uint32_t domainIndex = 0; domainIndex < domainCount; ++domainIndex)
    {
        const bool isControl = isControlDomain(domainIndex);
        const auto initState = isControl ? DomainRecord_t::State::Active : DomainRecord_t::State::Free;
        const std::uint32_t initHolder = isControl ? 0u : NoPeer;

        // Truth copy + one shadow per group: same initial (state, holder); epoch=0 from memset.
        for (std::uint32_t copyIndex = 0; copyIndex < groupCount + 1; ++copyIndex)
        {
            auto* record = (copyIndex == 0)
                               ? geometry.getDomainRecord(domainIndex)
                               : geometry.getDomainRecordShadow(domainIndex, (copyIndex - 1) * ShadowGroupSize);
            record->magic = DomainRecord_t::Magic;
            record->setState(initState);
            record->holder = initHolder;
        }
    }

    for (std::uint32_t peerIndex = 0; peerIndex < peerCount; ++peerIndex)
    {
        geometry.getMemberSlot(peerIndex)->magic = Member_t::Magic;
        if constexpr (ProfileEnabled)
        {
            geometry.getProfileSlot(peerIndex)->magic = MemberProfile_t::Magic;
        }
    }

    // Admission-control line: magic only; memset left nonce=0 (unlocked) and peerScanBound=0.
    geometry.getAdmissionControl()->magic = AdmissionControl_t::Magic;

    // RA-policy-private claim region (per-peer claim slots): magic + no-RA sentinel.
    if (const auto raPolicy = makeRecoveryAuthorityPolicy())
    {
        raPolicy->formatClaimRegion(geometry.getRecoveryAuthorityAreaBase(), peerCount);
    }

    // Strategy-specific region-resident metadata (e.g. RequestAgg aggregator records).
    if (const auto policy = makeSuccessorPolicy(opts.strategy))
    {
        policy->format(geometry.getSuccessorAreaBase(), domainCount, peerCount,
                       opts.aggregatorGroups, FormatCoherency);
    }

    // Publish slots before header magic -- any joiner that sees isFormatted()
    // must already see slot magics. Required on noncoherent CXL FAM.
    coherency::wmb(base, totalSize, FormatCoherency);

    CME_FAILPOINT_REACH(failpoint::Boundary::FormatBeforeHeader);

    // Header magic is the commit point. Build the line locally, then one whole-64B
    // set publishes it (format is the sole writer -- no field to preserve from FAM).
    Header_t header{};
    header.version = Header_t::Version;
    header.strategy = static_cast<std::uint32_t>(opts.strategy);
    header.aggregatorGroups = opts.aggregatorGroups;
    header.numDomains = domainCount;
    header.maxPeers = peerCount;
    header.formatGeneration = timing::wall<timing::Nanos>();
    header.totalSize = totalSize;
    // Control domain (0) is always live; data domains set their bit on create.
    DomainBitmap activeDomains;
    activeDomains.set(ControlDomainId);
    header.storeActiveDomains(activeDomains);
    header.magic = Header_t::Magic;

    coherency::set(geometry.getHeader(), header, FormatCoherency);
}

void Geometry::bindBlocking(timing::Millis timeout, CoherencyMode mode)
{
    if (bound_)
    {
        return;
    }
    void* const base = memory_->getBase();
    if (base == nullptr)
    {
        throw RegionInvalidError{"cme::Geometry::bindBlocking: region not mapped"};
    }
    if (!poll::waitUntil([base, mode]
                         {
                             return isHeaderReady(base, mode);
                         },
                         timeout, HeaderPollGap))
    {
        throw RegionNotFormattedError{
            "cme::Geometry::bindBlocking: region not formatted within timeout"};
    }
    auto* header = static_cast<Header_t*>(base);
    const std::uint32_t domainCount = header->numDomains;
    const std::uint32_t peerCount = header->maxPeers;
    const Strategy strategy = header->getStrategy();
    const std::uint32_t aggregatorGroups = header->getAggregatorGroups();
    // Header-supplied, so untrusted: another host wrote it. Above peerCount the group count
    // only inflates the computed area, which would then fail the mapping check below with a
    // misleading message.
    if (!isValidDomainCount(domainCount) || !isValidPeerCount(peerCount) ||
        aggregatorGroups > peerCount)
    {
        throw RegionInvalidError{"cme::Geometry::bindBlocking: invalid dims"};
    }
    if (memory_->getMappedSize() < computeAreaSize(domainCount, peerCount, strategy, aggregatorGroups))
    {
        throw RegionInvalidError{
            "cme::Geometry::bindBlocking: mappedSize smaller than area"};
    }
    buildLayout(domainCount, peerCount, strategy, aggregatorGroups);
    if (!isLayoutAligned())
    {
        throw RegionInvalidError{"cme::Geometry::bindBlocking: section base not 64B-aligned"};
    }
}

bool Geometry::rebind(Passkey<Inspector>, CoherencyMode mode) noexcept
{
    void* const base = memory_->getBase();
    if (!isHeaderReady(base, mode))
    {
        clearLayout();
        return false;
    }
    auto* header = static_cast<Header_t*>(base);
    const std::uint32_t domainCount = header->numDomains;
    const std::uint32_t peerCount = header->maxPeers;
    const Strategy strategy = header->getStrategy();
    const std::uint32_t aggregatorGroups = header->getAggregatorGroups();
    if (!isValidDomainCount(domainCount) || !isValidPeerCount(peerCount) ||
        aggregatorGroups > peerCount)
    {
        clearLayout();
        return false;
    }
    if (memory_->getMappedSize() < computeAreaSize(domainCount, peerCount, strategy, aggregatorGroups))
    {
        clearLayout();
        return false;
    }
    if (!bound_ || domainCount_ != domainCount || peerCount_ != peerCount || strategy_ != strategy)
    {
        buildLayout(domainCount, peerCount, strategy, aggregatorGroups);
    }
    if (!isLayoutAligned())
    {
        clearLayout();
        return false;
    }
    return true;
}

// ── private: ctors ─────────────────────────────────────────────────

Geometry::Geometry(std::unique_ptr<Memory> memory) noexcept
    : memory_{std::move(memory)}
{
}

Geometry::Geometry(std::unique_ptr<Memory> memory, std::uint32_t domainCount,
                   std::uint32_t peerCount, Strategy strategy, std::uint32_t aggregatorGroups)
    : memory_{std::move(memory)}
{
    if (!isValidDomainCount(domainCount) || !isValidPeerCount(peerCount))
    {
        throw FormatError{"cme::Geometry: invalid dims"};
    }
    if (memory_->getMappedSize() < computeAreaSize(domainCount, peerCount, strategy, aggregatorGroups))
    {
        throw FormatError{"cme::Geometry: mappedSize smaller than area"};
    }
    buildLayout(domainCount, peerCount, strategy, aggregatorGroups);
    if (!isLayoutAligned())
    {
        throw FormatError{"cme::Geometry: section base not 64B-aligned"};
    }
}

// ── private: layout helpers ────────────────────────────────────────

void Geometry::buildLayout(std::uint32_t domainCount, std::uint32_t peerCount,
                           Strategy strategy, std::uint32_t aggregatorGroups) noexcept
{
    domainCount_ = domainCount;
    peerCount_ = peerCount;
    strategy_ = strategy;

    auto* base = static_cast<std::uint8_t*>(memory_->getBase());
    std::uint64_t cursor = sizeof(Header_t);

    admissionControlBase_ = base + cursor;
    cursor += sizeof(AdmissionControl_t);

    // Replicated: getRecordsPerDomain copies per domain (1 truth + 1 shadow per group).
    domainRecordBase_ = base + cursor;
    cursor += static_cast<std::uint64_t>(domainCount) * getRecordsPerDomain(peerCount) * sizeof(DomainRecord_t);

    membershipBase_ = base + cursor;
    cursor += static_cast<std::uint64_t>(peerCount) * sizeof(Member_t);

    // ── policy-private regions (each sized/formatted by its policy) ──
    // RA-policy-private claim region: per-peer claim slots, sized by the RA policy.
    recoveryAuthorityAreaBase_ = base + cursor;
    cursor += getRecoveryAuthorityAreaSize(peerCount);

    // Strategy tail area: nullptr when this strategy needs none (size 0).
    const std::uint64_t succBytes = getSuccessorAreaSize(strategy, domainCount, peerCount, aggregatorGroups);
    successorAreaBase_ = (succBytes != 0) ? base + cursor : nullptr;
    cursor += succBytes;

    // Independent optional feature (CME_PROFILE), kept last as an appendix: off -> size 0
    // and the layout simply ends earlier, leaving no zero-length gap mid-region.
    // nullptr when off: getProfileSlot() returns nullptr, not one-past-end.
    profileBase_ = ProfileEnabled ? base + cursor : nullptr;
    cursor += getProfileAreaSize(domainCount, peerCount);

    areaSize_ = cursor;
    bound_ = true;
}

void Geometry::clearLayout() noexcept
{
    domainCount_ = 0;
    peerCount_ = 0;
    admissionControlBase_ = nullptr;
    domainRecordBase_ = nullptr;
    membershipBase_ = nullptr;
    recoveryAuthorityAreaBase_ = nullptr;
    successorAreaBase_ = nullptr;
    profileBase_ = nullptr;
    areaSize_ = 0;
    bound_ = false;
}

bool Geometry::isLayoutAligned() const noexcept
{
    const auto aligned = [](const void* addr) noexcept
    {
        return (reinterpret_cast<std::uintptr_t>(addr) & (CacheLineBytes - 1)) == 0;
    };
    // mmap gives a page-aligned base; each section is a run of 64B records, so every slot
    // lands on a cacheline boundary unless a section size is not a 64B multiple.
    if (!aligned(memory_->getBase()))
    {
        return false;
    }
    for (const std::uint8_t* sectionBase :
         {admissionControlBase_, domainRecordBase_, membershipBase_, recoveryAuthorityAreaBase_,
          successorAreaBase_, profileBase_})
    {
        if (sectionBase != nullptr && !aligned(sectionBase))
        {
            return false;
        }
    }
    return (areaSize_ & (CacheLineBytes - 1)) == 0;
}

}  // namespace cme
