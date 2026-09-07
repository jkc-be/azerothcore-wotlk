/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_WIRE_H
#define MOD_ALLES_WIRE_H

#include "interpreter/PilotCoordinator.h"
#include <boost/json.hpp>

namespace Alles::Bridge
{
boost::json::value Parse(std::string_view text);
boost::json::object EncodeJob(Interpreter::JobSnapshot const& job, uint64_t realMs);
Interpreter::ProposalEnvelope DecodeProposal(boost::json::object const& object);
std::string String(boost::json::object const& object, char const* key, std::size_t max = 128);
uint64_t Number(boost::json::object const& object, char const* key);
void Fields(boost::json::object const& object, std::initializer_list<char const*> fields);
} // namespace Alles::Bridge
#endif
