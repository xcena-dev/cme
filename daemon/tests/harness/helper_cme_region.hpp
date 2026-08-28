// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// helper_cme_region.hpp -- one cme region owned by one probe, for a case whose subject is the
// daemon's own half. Separate from helper.hpp because it drags in libcme.
//
// shm by default, so a case runs wherever ctest does. A case naming a uri instead owns that medium
// itself, down to the coherency mode it passes: nothing here unlinks a dax device or a file.

#pragma once

#include <sys/mman.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "cme/shared.hpp"
#include "harness/helper_medium.hpp"
#include "observe/inspector.hpp"

namespace cmed::harness
{

// Owns one cme region for as long as the probe runs. The leading unlink is not tidiness: a run that
// was killed leaves the shm name behind, and format would otherwise inherit its peer slots.
class ProbeRegion
{
public:
    // ── ctor / dtor ────────────────────────────────────────────────
    // @slots counts the control domain, so a case wanting two data domains asks for three. @peers
    // counts this session, so a case standing two daemons up beside it asks for three.
    ProbeRegion(const char* shmName, std::uint32_t slots, std::uint32_t peers = 2)
        : shmName_{shmName},
          uri_{std::string{"shm:"} + shmName},
          session_{freshSession(shmName, uri_, slots, peers)}
    {
    }

    // Any backend, named whole. Nothing is unlinked at either end: a dax device or a file belongs to
    // whoever set it up, and format is what makes the region this case's regardless.
    ProbeRegion(std::string uri, std::uint32_t slots, std::uint32_t peers,
                cme::CoherencyMode coherency)
        : shmName_{nullptr},
          uri_{std::move(uri)},
          session_{formattedSession(uri_, slots, peers, coherency)}
    {
    }

    ProbeRegion(const ProbeRegion&) = delete;
    ProbeRegion(ProbeRegion&&) = delete;

    // Only a name this fixture made. A uri the case named points at a medium it did not create.
    ~ProbeRegion() noexcept
    {
        if (shmName_ != nullptr)
        {
            ::shm_unlink(shmName_);
        }
    }

    // ── operator= ──────────────────────────────────────────────────
    ProbeRegion& operator=(const ProbeRegion&) = delete;
    ProbeRegion& operator=(ProbeRegion&&) = delete;

    // ── public methods ─────────────────────────────────────────────
    // The daemon joins on its first grant, so a case only creates. Creating from the same session
    // that will serve is what a real deployment does through the control channel.
    void createDomain(std::string_view name)
    {
        session_.createDomain(name);
    }

    // ── accessors ──────────────────────────────────────────────────
    [[nodiscard]] cme::Session& session() noexcept
    {
        return session_;
    }

private:
    [[nodiscard]] static cme::Session
    freshSession(const char* shmName, const std::string& uri, std::uint32_t slots, std::uint32_t peers)
    {
        ::shm_unlink(shmName);
        return formattedSession(uri, slots, peers, cme::CoherencyMode::CacheCoherent);
    }

    // The mode is the medium's, not a preference: a bare open defaults to CacheCoherent, and on
    // devdax or an uncacheable mount that is a barrier discipline the hardware does not have.
    [[nodiscard]] static cme::Session
    formattedSession(const std::string& uri, std::uint32_t slots, std::uint32_t peers,
                     cme::CoherencyMode coherency)
    {
        cme::Session::FormatOpts_t opts;
        opts.maxDomains = slots;
        opts.maxPeers = peers;
        cme::Session::format(uri, opts);

        cme::Session::OpenOpts_t opening;
        opening.coherency = coherency;
        return cme::Session::open(uri, opening);
    }

private:
    const char* shmName_;
    std::string uri_;
    cme::Session session_;
};

// ── what a region says from outside ─────────────────────────────
// Read through cme::Inspector rather than a session, because what a case asks here is what a
// survivor sees: a peer that died left its slot, and a session of its own would be a live peer.

// Whether the region still carries a domain under @name at all. Apart from the participant count
// because absent and unparticipated are different answers, and only one of them is a live domain.
[[nodiscard]] inline bool namePresent(const MediumOptions_t& chosen, const char* name)
{
    const auto watching = cme::Inspector::open(chosen.uri, chosen.coherencyMode());
    const auto header = watching.readHeader();
    if (!header.has_value())
    {
        return false;
    }

    for (std::uint32_t domainId = 0; domainId < header->numDomains; ++domainId)
    {
        const auto owned = watching.readOwnership(domainId);
        if (owned.has_value() && owned->name == name)
        {
            return true;
        }
    }
    return false;
}

// Peers carrying the participation bit for @name. What it answers is whether any node is in the
// domain, which is the fact a delete and a rejoin both turn on.
[[nodiscard]] inline std::uint32_t countParticipants(const MediumOptions_t& chosen, const char* name)
{
    const auto watching = cme::Inspector::open(chosen.uri, chosen.coherencyMode());
    const auto header = watching.readHeader();
    if (!header.has_value())
    {
        return 0;
    }

    for (std::uint32_t domainId = 0; domainId < header->numDomains; ++domainId)
    {
        const auto owned = watching.readOwnership(domainId);
        if (!owned.has_value() || owned->name != name)
        {
            continue;
        }

        std::uint32_t counted = 0;
        for (std::uint32_t peerId = 0; peerId < header->maxPeers; ++peerId)
        {
            const auto peer = watching.readPeer(peerId);
            if (peer.has_value() && peer->active && peer->participatingDomains.has(domainId))
            {
                ++counted;
            }
        }
        return counted;
    }

    return 0;
}

}  // namespace cmed::harness
