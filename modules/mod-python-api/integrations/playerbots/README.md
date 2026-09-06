# Playerbots integration

The adapter in `../../src/PlayerbotsAdapter.cpp` is wired to `mod-playerbots/mod-playerbots`, inspected at
revision `b949b50bfcdd4fab937781bac2d7765e39330e4b`. Playerbots remains responsible for spawning, sessions and logout.
The wrapper discovers configured online bots automatically, yields their normal AI to Python on claim, and restores
it on release or controller disconnect. It never registers a real player or a Playerbots selfbot.

Playerbots requires its own AzerothCore `Playerbot` branch. This fork's pinned submodule already includes the
integration, adapted to preserve Observatory instrumentation and native-client POV behavior.

## Prepare on the server side

For this fork, initialize the pinned submodule on a clean checkout and build both modules. Do not reapply the patch.
For another compatible checkout, copy `mod-python-api` beside its existing `mod-playerbots` directory, retaining
those exact directory names. From that server checkout root:

```sh
python3 modules/mod-python-api/integrations/playerbots/apply_patch.py modules/mod-playerbots
python3 modules/mod-python-api/integrations/playerbots/apply_patch.py modules/mod-playerbots --apply
```

The first command checks only. The second applies source changes. Neither command fetches, changes branches,
builds, installs or starts anything. The helper accepts compatible revisions when every patch hunk matches, refuses
local modifications to the target files, and detects an already-applied patch. It does not overwrite a different
Playerbots version to force a match. On a mismatch, rebase the patch against that server's module revision.

Build both modules statically using the server's usual procedure. If the adapter finds Playerbots headers without
the external-control patch, compilation stops with an explicit instruction to apply it. Missing Playerbots headers
produce a standalone wrapper with an explicit startup warning instead of pretending the integration is available.

Set the effective server configuration:

```ini
PythonAPI.Enable = 1
PythonAPI.Port = 5001
PythonAPI.Playerbots.Names = "Bota,Botb"
```

Use your existing Playerbots commands to log those characters in. Within roughly one second, `Client.list_bots()`
will expose their raw GUIDs. No GM command or manually written C++ registration callback is needed. An empty name
list exposes no bots. Each Python environment claims one GUID from this list.

## What the Playerbots patch changes

- Adds explicit external-control state and a unique identity per `PlayerbotAI` instance.
- Suppresses normal AI decisions, bot cheats, chat/remote commands and AI-level casts/movement while claimed.
- Clears bot decision/packet queues at ownership changes. Discards queued world operations belonging to claimed
  bots and guards delayed AI callbacks against ownership changes and replacement AI instances.
- Suspends the random-bot scheduler's per-bot processing while claimed, preventing its routine teleport, revive,
  gear randomization and scheduled per-bot logout paths from changing a training character.
- Leaves session updates and `HandleTeleportAck()` operational. Server shutdown, explicit GM actions, master
  logout and global Playerbots logout policies can still remove a bot; those invalidate the wrapper's binding.

The ownership handoff resets cached AI decisions, stops current movement/casts and combat with pets. Release
reinitializes normal decisions; it does not replay the commands discarded at claim time. Other characters, creatures
and ordinary server gameplay continue updating. This is not a deterministic or isolated simulation.

## Reset scope

On every successful claim, the adapter records that bot's current map, instance and position as its reset origin.
`reset()` is a **character reset on outdoor maps**: stop combat with pets and current casting/movement, resurrect
when dead, clear player auras and spell cooldowns, refill health/current power, and teleport to that origin.
It uses Playerbots' own teleport acknowledgment method, then checks that the bot is alive and in world at the origin
before acknowledging the new episode. Taxi, vehicle, transport and instanced reset scenarios are rejected.

It does not rewind level, XP, inventory, quests, nearby spawns, loot, threat history elsewhere, or pet health/cooldowns.
Those changes can still be saved by normal server persistence. Configure a disposable training character/scenario
appropriate to this reset scope; no character/database creation is performed by this integration.

Bindings use the AI instance's generation, not just its player's GUID. Logout, AI replacement and switching to a
selfbot invalidate pending observations before the wrapper can act on a new owner of the same GUID. Discovery
removes stale registrations and can register a later headless incarnation as a new observation baseline.

## Validation boundary

The integrated pin builds with the core and passes CTest; Python transport tests exercise the wrapper contract.
Enabled API gameplay validation remains pending. Server verification must cover claim/release, dead reset,
far/near teleports, disconnect during reset,
logout/relogin with the same GUID, and restoration of ordinary Playerbots behavior with the intended map workers.

Upstream source: https://github.com/mod-playerbots/mod-playerbots/tree/b949b50bfcdd4fab937781bac2d7765e39330e4b
The patched files retain their existing GPLv2-or-later notices. Attribution is recorded in `../../SOURCES.md`.
