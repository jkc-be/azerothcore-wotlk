/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef CHARACTER_DATABASE_STATEMENT_LAYOUT_H
#define CHARACTER_DATABASE_STATEMENT_LAYOUT_H

#include "CharacterDatabase.h"
#include <string_view>

namespace Acore::Impl
{
    struct CharacterStatementDescriptor
    {
        std::string_view name;
        uint32 index;
    };

    // Internal linkage keeps deliberately mismatched consumers independent from the library.
    static constexpr CharacterDatabaseStatementLayout CompiledCharacterDatabaseStatementLayout()
    {
        constexpr CharacterStatementDescriptor statements[] =
        {
#include "CharacterDatabaseStatementDescriptors.inc"
        };
        static_assert(sizeof(statements) / sizeof(statements[0]) == MAX_CHARACTERDATABASE_STATEMENTS);

        uint64 fingerprint = 14695981039346656037ULL;
        for (auto const& statement : statements)
        {
            for (char byte : statement.name)
            {
                fingerprint ^= static_cast<uint8>(byte);
                fingerprint *= 1099511628211ULL;
            }

            // Name terminator followed by a fixed-width little-endian index.
            fingerprint *= 1099511628211ULL;
            for (uint32 shift = 0; shift < 32; shift += 8)
            {
                fingerprint ^= (statement.index >> shift) & 0xff;
                fingerprint *= 1099511628211ULL;
            }
        }

        return {MAX_CHARACTERDATABASE_STATEMENTS, fingerprint};
    }
}

#endif
