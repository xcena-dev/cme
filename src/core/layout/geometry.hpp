// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// geometry.hpp -- on-disk record types + Geometry view.
//
// All records are 64 B ("CMEx" magic). open() returns unbound; create() bound.
// rebind() re-validates header + recomputes pointers (Inspector path).
// MemberProfile_t is in core/layout/geometry_profile.hpp.
// Inv1: by-construction for per-node lines; derived for DomainRecord_t.

#pragma once

#include <string.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "cme/shared.hpp"
#include "common/timing.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry_profile.hpp"
#include "core/policy/recovery_authority.hpp"
#include "core/policy/successor.hpp"
#include "core/types.hpp"
#include "memory/memory.hpp"
#include "util/endian.hpp"
#include "util/util.hpp"

namespace cme
{

class Inspector;

class Geometry
{
public:
    // ── on-disk records (each 64 B, one cacheline) ──────────────────

    struct Header_t
    {
        static constexpr std::uint32_t Magic = 0x434D4548u;  // "CMEH"
        static constexpr std::uint32_t Version = 1;

        endian::Field_t<std::uint32_t> magic;                           //  0
        endian::Field_t<std::uint32_t> version;                         //  4
        endian::Field_t<std::uint32_t> strategy;                        //  8: cme::Strategy cast to u32
        endian::Field_t<std::uint32_t> numDomains;                      // 12: slot ceiling (control + data), NOT live count
        endian::Field_t<std::uint32_t> maxPeers;                        // 16
        endian::Field_t<std::uint32_t> aggregatorGroups;                // 20: RequestAgg group count (0 = auto / other strategies)
        endian::Field_t<std::uint64_t> formatGeneration;                // 24: bumped each format()
        endian::Field_t<std::uint64_t> totalSize;                       // 32: cached getAreaSize() result
        endian::Field_t<std::uint64_t> activeDomains[DomainWordCount];  // 40: live-domain bitmap

        // Bytes the fields above spend. Asserted before the subtraction, because the operands
        // are unsigned: a bitmap that outgrew the line would wrap this into a huge array and
        // report a size no one can trace back to MaxDomains.
        static constexpr std::uint32_t NamedBytes =
            40 + sizeof(endian::Field_t<std::uint64_t>) * DomainWordCount;
        static_assert(NamedBytes < 64, "Header_t: activeDomains leaves no pad; lower MaxDomains");

        std::uint8_t reserved[64 - NamedBytes];  // pad to 64 B

        [[nodiscard]] bool isValidMagic() const noexcept
        {
            return static_cast<std::uint32_t>(magic) == Magic;
        }
        [[nodiscard]] bool isSupportedVersion() const noexcept
        {
            return static_cast<std::uint32_t>(version) == Version;
        }
        [[nodiscard]] bool isFormatted() const noexcept
        {
            return isValidMagic() && isSupportedVersion();
        }
        [[nodiscard]] Strategy getStrategy() const noexcept
        {
            return static_cast<Strategy>(static_cast<std::uint32_t>(strategy));
        }
        [[nodiscard]] std::uint32_t getAggregatorGroups() const noexcept
        {
            return aggregatorGroups;
        }
        [[nodiscard]] DomainBitmap loadActiveDomains() const noexcept
        {
            DomainBitmap bits;
            for (std::uint32_t word = 0; word < DomainWordCount; ++word)
            {
                bits.setWord(word, activeDomains[word]);
            }
            return bits;
        }
        void storeActiveDomains(DomainBitmap bits) noexcept
        {
            for (std::uint32_t word = 0; word < DomainWordCount; ++word)
            {
                activeDomains[word] = bits.getWord(word);
            }
        }
    };

    // Admission plane: a nonce lease serialises peer-slot allocation atomic-free, and
    // peerScanBound caps membership scans. Kept off the control plane's lines so a lock-free
    // joiner and a control-lock holder never share a cacheline.
    struct AdmissionControl_t
    {
        static constexpr std::uint32_t Magic = 0x434D4F43u;  // "CMOC"

        endian::Field_t<std::uint32_t> magic;          //  0
        endian::Field_t<std::uint32_t> peerScanBound;  //  4: claimed-slot ceiling; bounds peer scans
        endian::Field_t<std::uint64_t> nonce;          //  8: lease holder token; 0 = unlocked
        std::uint8_t reserved[48];                     // 16..63

        [[nodiscard]] bool isValidMagic() const noexcept
        {
            return static_cast<std::uint32_t>(magic) == Magic;
        }
    };

    // Per-domain line: SWOT record (holder/epoch) + registry (state/name).
    // Create publishes state=Active as the line's last write; delete requires
    // domain holder + control holder (TLC: spec/SWOT_control_data.tla).
    struct DomainRecord_t
    {
        static constexpr std::uint32_t Magic = 0x434D4554u;  // "CMET"
        static constexpr std::size_t MaxNameLen = 16;

        // Active = live domain; Free = reusable spare. createDomain fills Free then
        // flips Active as last write; a mid-write crash leaves it Free (safely reclaimable).
        enum class State : std::uint32_t
        {
            Free = 0,    // spare entry: no live domain here, reusable
            Active = 1,  // live domain
        };

        endian::Field_t<std::uint32_t> magic;       //  0
        endian::Field_t<std::uint32_t> state;       //  4: State
        endian::Field_t<std::uint64_t> generation;  //  8: ++ per create (slot-reuse ABA guard)
        endian::Field_t<std::uint64_t> epoch;       // 16: SWOT epoch; ++ on every transfer
        endian::Field_t<std::uint32_t> holder;      // 24: PeerId; always a definite holder
        char name[MaxNameLen];                      // 28..43: NUL-terminated
        std::uint8_t reserved[20];                  // 44..63

        [[nodiscard]] bool isValidMagic() const noexcept
        {
            return static_cast<std::uint32_t>(magic) == Magic;
        }
        [[nodiscard]] State getState() const noexcept
        {
            return static_cast<State>(static_cast<std::uint32_t>(state));
        }
        void setState(State value) noexcept
        {
            state = static_cast<std::uint32_t>(value);
        }
        [[nodiscard]] bool hasState(State expected) const noexcept
        {
            return getState() == expected;
        }
        // Name as a string_view (fixed-width field, NUL-terminated).
        [[nodiscard]] std::string_view getName() const noexcept
        {
            return std::string_view{name, ::strnlen(name, MaxNameLen)};
        }
        [[nodiscard]] PeerId getHolder() const noexcept
        {
            return static_cast<PeerId>(holder);
        }
        [[nodiscard]] bool isHeldBy(PeerId peer) const noexcept
        {
            return getHolder() == peer;
        }
    };

    // SWPC per-peer line, written by the peer it indexes (Inv1 by construction). Core
    // membership only -- status, liveness witness, participation -- so the slot is
    // policy-blind; the REQUEST demand signal lives in the successor area.
    struct Member_t
    {
        static constexpr std::uint32_t Magic = 0x434D454Du;  // "CMEM"
        // Member states (report §4.4).
        enum class Status : std::uint32_t
        {
            None = 0,        // free slot; the only status admission re-admits
            Active = 1,      // joined and selectable; a crash leaves the slot here
            Recovering = 2,  // RA-seized after a crash; not selectable, not re-admittable
            Leaving = 3,     // clean departure draining; still forwards, not selectable
        };

        endian::Field_t<std::uint32_t> magic;
        endian::Field_t<std::uint32_t> status;         // Member_t::Status
        endian::Field_t<std::uint64_t> lastSeenNanos;  // LivenessPolicy witness: wall-clock ns of last self-stamp
        // Participation bitmap: poll-scan scoping. Control bit always set; recovery scrubs dead peer's data bits.
        endian::Field_t<std::uint64_t> participatingDomains[DomainWordCount];
        // No generation counter: self owns its slot and reads its own state from DRAM; cross-host
        // zombie fencing is the platform's forced FAM-revoke (marufs), not cooperative gen polling.

        // Same guard as Header_t: the pad is a subtraction on unsigned operands, so the fit has
        // to be asserted rather than assumed.
        static constexpr std::uint32_t NamedBytes =
            16 + sizeof(endian::Field_t<std::uint64_t>) * DomainWordCount;
        static_assert(NamedBytes < 64,
                      "Member_t: participatingDomains leaves no pad; lower MaxDomains");

        std::uint8_t reserved[64 - NamedBytes];

        [[nodiscard]] bool isValidMagic() const noexcept
        {
            return static_cast<std::uint32_t>(magic) == Magic;
        }
        [[nodiscard]] bool hasStatus(Status expected) const noexcept
        {
            return static_cast<std::uint32_t>(status) == static_cast<std::uint32_t>(expected);
        }
        [[nodiscard]] bool isRecovering() const noexcept
        {
            return hasStatus(Status::Recovering);
        }
        void setStatus(Status value) noexcept
        {
            status = static_cast<std::uint32_t>(value);
        }
        [[nodiscard]] DomainBitmap loadParticipatingDomains() const noexcept
        {
            DomainBitmap bits;
            for (std::uint32_t word = 0; word < DomainWordCount; ++word)
            {
                bits.setWord(word, participatingDomains[word]);
            }
            return bits;
        }
        void storeParticipatingDomains(DomainBitmap bits) noexcept
        {
            for (std::uint32_t word = 0; word < DomainWordCount; ++word)
            {
                participatingDomains[word] = bits.getWord(word);
            }
        }
    };

    // The per-dead-peer recovery claim word is not a core-geometry section: it lives in an
    // RA-policy-private region (recovery_authority.cpp), sized/formatted by
    // RecoveryAuthorityPolicy -- see getRecoveryAuthorityAreaBase().

    static_assert(sizeof(Header_t) == 64, "Header_t must be 64 B");
    static_assert(sizeof(AdmissionControl_t) == 64, "AdmissionControl_t must be 64 B");
    static_assert(sizeof(DomainRecord_t) == 64, "DomainRecord_t must be 64 B");
    static_assert(sizeof(Member_t) == 64, "Member_t must be 64 B");

    struct FormatOpts_t
    {
        Strategy strategy;
        std::uint32_t aggregatorGroups{0};  // RequestAgg group count (0 = auto)
    };

    // ── static helpers ──────────────────────────────────────────────

    // One truth copy plus a shadow per peer-group. A waiter polls only its group's shadow,
    // spreading the wait instead of hammering one line; ShadowGroupSize caps the read storm.
    static constexpr std::uint32_t ShadowGroupSize = 1;

    [[nodiscard]] static constexpr std::uint32_t getGroupCount(std::uint32_t peerCount) noexcept
    {
        return (peerCount + ShadowGroupSize - 1) / ShadowGroupSize;
    }
    [[nodiscard]] static constexpr std::uint32_t getGroupIndex(std::uint32_t peerIndex) noexcept
    {
        return peerIndex / ShadowGroupSize;
    }
    // DomainRecord_t copies per domain: truth (index 0) + one shadow per group.
    [[nodiscard]] static constexpr std::uint32_t getRecordsPerDomain(std::uint32_t peerCount) noexcept
    {
        return getGroupCount(peerCount) + 1;
    }

    // Not constexpr: the per-strategy tail size is delegated to the policy object
    // (getSuccessorAreaSize), which is a runtime call. Only used on format/bind.
    [[nodiscard]] static std::uint64_t
    computeAreaSize(std::uint32_t domainCount, std::uint32_t peerCount, Strategy strategy,
                    std::uint32_t aggregatorGroups) noexcept
    {
        return sizeof(Header_t) + sizeof(AdmissionControl_t) +
               static_cast<std::uint64_t>(domainCount) * getRecordsPerDomain(peerCount) * sizeof(DomainRecord_t) +
               static_cast<std::uint64_t>(peerCount) * sizeof(Member_t) +
               getRecoveryAuthorityAreaSize(peerCount) +
               getProfileAreaSize(domainCount, peerCount) +
               getSuccessorAreaSize(strategy, domainCount, peerCount, aggregatorGroups);
    }

    // ── rule of five ────────────────────────────────────────────────

    Geometry() = delete;
    Geometry(const Geometry&) = delete;
    Geometry& operator=(const Geometry&) = delete;
    Geometry(Geometry&&) noexcept = default;
    Geometry& operator=(Geometry&&) noexcept = default;
    ~Geometry() = default;

    // ── factories ───────────────────────────────────────────────────

    [[nodiscard]] static Geometry open(std::string_view uri);
    [[nodiscard]] static Geometry create(std::string_view uri, std::uint32_t domainCount,
                                         std::uint32_t peerCount, const FormatOpts_t& opts);

    // ── ops ─────────────────────────────────────────────────────────

    void format(const FormatOpts_t& opts);  // geometry must be bound first

    // Poll header up to `timeout`, then bind. Throws on timeout / invalid header.
    // @mode is this peer's regime; bindBlocking polls the header for the creator's format.
    void bindBlocking(timing::Millis timeout, CoherencyMode mode);

    // Inspector path: best-effort re-validate; picks up reformats.
    // Returns false on invalid header. Passkey-protected.
    [[nodiscard]] bool rebind(Passkey<Inspector>, CoherencyMode mode) noexcept;

    // ── accessors ───────────────────────────────────────────────────
    // Valid only after open()->bindBlocking() or rebind().

    [[nodiscard]] std::uint32_t getDomainCount() const noexcept
    {
        return domainCount_;
    }
    [[nodiscard]] std::uint32_t getPeerCount() const noexcept
    {
        return peerCount_;
    }
    [[nodiscard]] Strategy getStrategy() const noexcept
    {
        return strategy_;
    }
    // Base of this strategy's trailing metadata area; nullptr when the strategy
    // needs none (Order/Request). Policy impl casts to its own structure.
    [[nodiscard]] std::uint8_t* getSuccessorAreaBase() const noexcept
    {
        return successorAreaBase_;
    }
    [[nodiscard]] Header_t* getHeader() const noexcept
    {
        return reinterpret_cast<Header_t*>(memory_->getBase());
    }
    [[nodiscard]] AdmissionControl_t* getAdmissionControl() const noexcept
    {
        return reinterpret_cast<AdmissionControl_t*>(admissionControlBase_);
    }
    // Authoritative truth copy (index 0 of this domain's replicated block).
    [[nodiscard]] DomainRecord_t* getDomainRecord(std::uint32_t domainIndex) const noexcept
    {
        const std::uint64_t flatIndex =
            static_cast<std::uint64_t>(domainIndex) * getRecordsPerDomain(peerCount_);
        return getSlot<DomainRecord_t>(domainRecordBase_, flatIndex);
    }
    // Shadow copy for the group @peerIndex belongs to (truth is index 0, shadows 1 + group).
    [[nodiscard]] DomainRecord_t* getDomainRecordShadow(std::uint32_t domainIndex,
                                                        std::uint32_t peerIndex) const noexcept
    {
        const std::uint64_t flatIndex =
            static_cast<std::uint64_t>(domainIndex) * getRecordsPerDomain(peerCount_) + 1 + getGroupIndex(peerIndex);
        return getSlot<DomainRecord_t>(domainRecordBase_, flatIndex);
    }
    [[nodiscard]] Member_t* getMemberSlot(std::uint32_t peerIndex) const noexcept
    {
        return getSlot<Member_t>(membershipBase_, peerIndex);
    }
    // Base of the RA-policy-private claim region (per-peer claim slots). The RA policy
    // casts it to its own slot struct; core geometry stays blind to the layout.
    [[nodiscard]] std::uint8_t* getRecoveryAuthorityAreaBase() const noexcept
    {
        return recoveryAuthorityAreaBase_;
    }
    [[nodiscard]] MemberProfile_t* getProfileSlot(std::uint32_t peerIndex) const noexcept
    {
        return getSlot<MemberProfile_t>(profileBase_, peerIndex);
    }

private:
    // Open-mode ctor leaves Geometry unbound; creator-mode commits dims.
    explicit Geometry(std::unique_ptr<Memory> memory) noexcept;
    Geometry(std::unique_ptr<Memory> memory, std::uint32_t domainCount,
             std::uint32_t peerCount, Strategy strategy, std::uint32_t aggregatorGroups);

    template <typename T>
    [[nodiscard]] static T* getSlot(std::uint8_t* sectionBase, std::uint64_t index) noexcept
    {
        return sectionBase == nullptr
                   ? nullptr
                   : reinterpret_cast<T*>(sectionBase + index * sizeof(T));
    }

    void buildLayout(std::uint32_t domainCount, std::uint32_t peerCount, Strategy strategy,
                     std::uint32_t aggregatorGroups) noexcept;
    void clearLayout() noexcept;
    // Every section base and the total size must be 64B-aligned, since get/set issue
    // whole-line transactions. Holds by construction; the check guards a future non-64B
    // record or section size.
    [[nodiscard]] bool isLayoutAligned() const noexcept;

    std::unique_ptr<Memory> memory_;
    std::uint32_t domainCount_{0};
    std::uint32_t peerCount_{0};
    Strategy strategy_{Strategy::Request};
    std::uint8_t* admissionControlBase_{nullptr};
    std::uint8_t* domainRecordBase_{nullptr};
    std::uint8_t* membershipBase_{nullptr};
    std::uint8_t* recoveryAuthorityAreaBase_{nullptr};
    std::uint8_t* successorAreaBase_{nullptr};
    std::uint8_t* profileBase_{nullptr};
    std::uint64_t areaSize_{0};
    bool bound_{false};
};

}  // namespace cme
