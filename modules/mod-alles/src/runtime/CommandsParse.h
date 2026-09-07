/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_COMMANDS_PARSE_H
#define MOD_ALLES_COMMANDS_PARSE_H

#include "domain/Memory.h"
#include <array>
#include <charconv>

namespace Alles::Commands
{
struct Arguments
{
    std::array<std::string_view, 2> tokens;
    std::size_t count = 0;
};

// Bounded legacy-handler input, with no command links, signs, hexadecimal IDs or trailing tokens.
inline std::optional<Arguments> ParseArguments(char const* args)
{
    if (!args)
        return Arguments{};
    std::size_t length = 0;
    while (length <= 128 && args[length] != '\0')
        ++length;
    if (length > 128)
        return std::nullopt;

    Arguments result;
    std::string_view const text(args, length);
    std::size_t offset = 0;
    while (offset < text.size())
    {
        if (text[offset] == ' ' || text[offset] == '\t')
        {
            ++offset;
            continue;
        }
        if (result.count == result.tokens.size())
            return std::nullopt;
        auto const begin = offset;
        while (offset < text.size() && text[offset] != ' ' && text[offset] != '\t')
            ++offset;
        result.tokens[result.count++] = text.substr(begin, offset - begin);
    }
    return result;
}

inline std::optional<uint64_t> ParsePositiveId(std::string_view text)
{
    if (text.empty() || text.front() < '0' || text.front() > '9')
        return std::nullopt;
    uint64_t value = 0;
    auto const result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0)
        return std::nullopt;
    return value;
}

inline std::optional<std::size_t> RecallCount(Arguments const& args)
{
    if (args.count == 0)
        return 5;
    if (args.count != 1)
        return std::nullopt;
    auto const number = ParsePositiveId(args.tokens[0]);
    if (!number || *number > 20)
        return std::nullopt;
    return static_cast<std::size_t>(*number);
}

inline std::optional<ActorKey> Owner(Arguments const& args)
{
    if (args.count != 2 || (args.tokens[0] != "player" && args.tokens[0] != "creature"))
        return std::nullopt;
    auto const id = ParsePositiveId(args.tokens[1]);
    if (!id)
        return std::nullopt;
    return ActorKey{args.tokens[0] == "player" ? ActorKind::Player : ActorKind::CreatureSpawn, *id};
}
}

#endif
