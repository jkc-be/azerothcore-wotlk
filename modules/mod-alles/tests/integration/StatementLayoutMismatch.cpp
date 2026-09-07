/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

// Deliberately violate the database target's PUBLIC definition in this consumer only.
// The database library and ordinary module remain compiled with MOD_ALLES.
#undef MOD_ALLES
#include "AllesDatabaseGuard.h"

int main()
{
    Alles::CheckCharacterStatementLayout();
    return 0;
}
