// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// inspector.hpp -- read-only region observer for monitoring / forensic dumps.
// Rebinds Geometry before each sample; returns nullopt on invalid windows.
// PeerSnapshot_t merges membership + profile. Field_t names use ME vocabulary.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cme/shared.hpp"
#include "core/domain_bitmap.hpp"
#include "core/layout/geometry.hpp"
#include "core/layout/geometry_profile.hpp"
#include "core/types.hpp"

namespace cme
{

class Inspector
{
public:
    struct HeaderSnapshot_t
    {
        Strategy strategy;
        DomainId numDomains;
        PeerId maxPeers;
        std::uint64_t formatGeneration;
        std::uint64_t totalSize;
    };

    struct OwnershipSnapshot_t
    {
        PeerId holder;
        std::uint64_t epoch;
        std::string name;  // as stored in the record, empty when the domain was never named
    };

    // Merged membership + profile view.
    struct PeerSnapshot_t
    {
        bool active;
        PeerId selfId;
        std::uint64_t lastSeenNanos;
        DomainBitmap pendingDomains;
        ProfileTimes_t time;  // all-zero if profile cacheline invalid
    };

    // ── rule of five ───────────────────────────────────────────────
    Inspector(const Inspector&) = delete;
    Inspector& operator=(const Inspector&) = delete;
    Inspector(Inspector&& other) noexcept = default;
    Inspector& operator=(Inspector&& other) noexcept = default;
    ~Inspector() = default;

    // ── factories ──────────────────────────────────────────────────
    [[nodiscard]] static Inspector open(std::string_view uri, CoherencyMode coherency);

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] std::optional<HeaderSnapshot_t> readHeader() const;
    [[nodiscard]] std::optional<OwnershipSnapshot_t> readOwnership(DomainId domainId) const;
    [[nodiscard]] std::optional<PeerSnapshot_t> readPeer(PeerId peerId) const;

private:
    Inspector(Geometry geometry, CoherencyMode coherency) noexcept;

    mutable Geometry geometry_;
    CoherencyMode coherency_;
};

}  // namespace cme
