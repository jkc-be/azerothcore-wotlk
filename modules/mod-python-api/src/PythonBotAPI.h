#ifndef MOD_PYTHON_BOT_API_H
#define MOD_PYTHON_BOT_API_H

#include "ObjectGuid.h"
#include <functional>

namespace PythonAPI
{
// The existing bot provider owns the Player, WorldSession and normal AI throughout.
// Call these functions at a world-update boundary, after map workers have finished.
// Callbacks run at that same boundary. Capture GUIDs, never long-lived Player pointers.
struct BotBinding
{
    // true: suspend normal AI; false: resume it. Return false to refuse a claim.
    // Releasing must succeed. Callbacks must not throw or re-enter this API.
    std::function<bool(bool)> setControlled;

    // Optional, provided together. Begin an episode reset and report when it is complete.
    // The provider handles resurrection, state restoration and headless teleport acknowledgments.
    std::function<bool()> beginReset;
    std::function<bool()> resetReady;

    // Optional lifetime check, evaluated before resolving/mutating a player. Providers with
    // independently managed sessions use it to reject replacement AIs with the same player GUID.
    std::function<bool()> isAvailable;
};

// Register only provider-owned headless bots. Call UnregisterBot BEFORE logout/session deletion
// or replacement, even when the replacement character has the same GUID.
bool RegisterBot(ObjectGuid guid, BotBinding binding);
void UnregisterBot(ObjectGuid guid);
}

#endif
