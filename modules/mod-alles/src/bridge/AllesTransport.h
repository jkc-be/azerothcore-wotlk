/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_TRANSPORT_H
#define MOD_ALLES_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Alles::Bridge
{
struct Frame
{
    uint64_t connection = 0; // Zero identifies a durable ledger completion, never a network client.
    std::string text;
};

// Socket and filesystem work owns values only. The world polls a bounded mailbox.
class Transport
{
public:
    Transport(uint16_t port, std::string ledger, std::string profile, uint32_t maxRequests);
    ~Transport();
    std::vector<Frame> Poll();
    void Reply(uint64_t connection, std::string text);
    void Reserve(std::string permit, std::string record);
    void Publish(std::string directory, std::string snapshot, std::string manifest);
    uint32_t Charged() const;
    std::vector<uint64_t> RecentReservations() const;
    uint16_t Port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace Alles::Bridge
#endif
