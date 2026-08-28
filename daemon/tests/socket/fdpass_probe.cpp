// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// fdpass_probe.cpp -- handing a mapping over a connection instead of naming it.
//
// The daemon creates an anonymous object and passes the descriptor to whoever it admitted, so
// nothing is left in a namespace for a third process to open or create first.
//
// A receiver not prepared for descriptors refuses the message instead of silently dropping them.

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <system_error>

#include "harness/helper_socket.hpp"
#include "shared/posix/mem_file.hpp"
#include "shared/posix/seqpacket_socket.hpp"
#include "shared/posix/unique_fd.hpp"

namespace
{

constexpr const char* SocketPath = "fdpass_probe.sock";
constexpr mode_t SocketMode = 0600;
constexpr std::uint64_t Bytes = 4096;
constexpr std::uint32_t MostBytes = 256;
constexpr const char* Marker = "registry";

// The area as the daemon publishes it: written through its own mapping, with the descriptor being
// what travels rather than a name.
[[nodiscard]] posix::MemFile publishedArea()
{
    auto area = posix::MemFile::create("cmed-fdpass-probe", Bytes);
    std::strncpy(static_cast<char*>(area.base()), Marker, Bytes - 1);
    return area;
}

[[nodiscard]] std::string readThrough(posix::FileDesc descriptor)
{
    void* const reading = ::mmap(nullptr, Bytes, PROT_READ, MAP_SHARED, descriptor, 0);
    if (reading == MAP_FAILED)
    {
        return {};
    }

    const std::string seen{static_cast<const char*>(reading)};
    static_cast<void>(::munmap(reading, Bytes));
    return seen;
}

// One message, one descriptor, and the bytes beside it. The receiver maps what arrived and finds what
// the sender wrote, which is the whole of what replaces looking a name up.
bool carriesADescriptorAcross()
{
    cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(SocketPath, SocketMode);
    const posix::MemFile area = publishedArea();

    if (!pair.serving.send("welcome", {area.descriptor()}))
    {
        std::printf("the send did not go\n");
        return false;
    }

    const std::optional<posix::SeqpacketSocket::Received_t> received = pair.asking.receive(MostBytes, 1);
    if (!received || received->descriptors.size() != 1)
    {
        std::printf("nothing arrived, or not one descriptor\n");
        return false;
    }

    return received->message == "welcome" && readThrough(received->descriptors.front().get()) == Marker;
}

// The ordinary message. Most of what crosses this socket carries nothing but bytes.
bool aMessageWithoutDescriptorsCarriesNone()
{
    cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(SocketPath, SocketMode);
    if (!pair.serving.send("answer", {}))
    {
        return false;
    }

    const std::optional<posix::SeqpacketSocket::Received_t> received = pair.asking.receive(MostBytes, 1);
    return received && received->message == "answer" && received->descriptors.empty();
}

// A receiver expecting none is told, rather than quietly handed a message whose descriptors the
// kernel dropped on the floor.
bool refusesWhatItCannotHold()
{
    cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(SocketPath, SocketMode);
    const posix::MemFile area = publishedArea();
    static_cast<void>(pair.serving.send("welcome", {area.descriptor()}));

    try
    {
        static_cast<void>(pair.asking.receive(MostBytes, 0));
    }
    catch (const std::system_error&)
    {
        return true;
    }
    std::printf("a message with a descriptor was accepted by a receiver expecting none\n");
    return false;
}

// The connection closing is how a departure is seen. Under the design this is also how a requester's
// death is seen, which is why it has to read as nothing rather than as an error.
bool nothingArrivesFromAPeerThatLeft()
{
    cmed::harness::ConnectedPair_t pair = cmed::harness::connectSocketPair(SocketPath, SocketMode);
    pair.serving = posix::SeqpacketSocket{};

    return !pair.asking.receive(MostBytes, 1);
}

}  // namespace

int main()
{
    ::unlink(SocketPath);

    bool passed = false;
    try
    {
        passed = carriesADescriptorAcross() && aMessageWithoutDescriptorsCarriesNone() &&
                 refusesWhatItCannotHold() && nothingArrivesFromAPeerThatLeft();
    }
    catch (const std::exception& failure)
    {
        std::printf("fdpass probe threw: %s\n", failure.what());
        return 1;
    }

    ::unlink(SocketPath);
    std::printf("fdpass probe %s\n", passed ? "ok" : "failed");
    return passed ? 0 : 1;
}
