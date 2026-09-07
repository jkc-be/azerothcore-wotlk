/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef MOD_ALLES_DATABASE_GUARD_H
#define MOD_ALLES_DATABASE_GUARD_H

#include "CharacterDatabaseStatementLayout.h"
#include "Errors.h"

namespace Alles
{
    // Each consumer compares its own compiled header with the database library's export.
    static inline void CheckCharacterStatementLayout()
    {
        constexpr auto expected = Acore::Impl::CompiledCharacterDatabaseStatementLayout();
        auto const actual = GetCharacterDatabaseStatementLayout();
        if (actual.count != expected.count || actual.fingerprint != expected.fingerprint)
            ABORT("mod-alles CharacterDatabase statement layout mismatch: module count={} fingerprint={}, "
                "database count={} fingerprint={}. Rebuild all consumers with consistent MOD_ALLES definitions.",
                expected.count, expected.fingerprint, actual.count, actual.fingerprint);
    }
}

#endif
