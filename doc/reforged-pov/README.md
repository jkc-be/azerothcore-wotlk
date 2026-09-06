# Reforged POV server handoff

This branch adds a GM spectator prototype with an online character picker and a live HUD in the
WoW 3.3.5a client. The client uses native world rendering and bind sight. Online server `Player`
objects are eligible, including simulated players that use that representation.

The extension is integrated into this fork's Playerbots master. Build with Custom scripts enabled.
The server command path must dispatch permitted commands before the spectator chat restriction, so
`.pov stop` and target switching remain available while observing.

The pinned `modules/mod-playerbots` submodule already includes the observer compatibility changes and the
observatory instrumentation. Run `git submodule update --init --recursive` on a clean checkout; do not apply
the bundled patch again to this pin. The patch remains as a reference for the original upstream module revision
`b949b50bfcdd4fab937781bac2d7765e39330e4b`. When migrating a deployment with a manually patched module,
follow [the agent update procedure](../../.agents/docs/systems/observatory.md) and preserve local changes first.

The compatibility code keeps a watched bot active despite GM invisibility and streams its successful AI actions.
Ordinary activity policy resumes when bind sight is detached. Native-client POV operates with the browser
observatory disabled, or explicitly opt in with `Observatory.AllowGmObservers = 1`. In that mode only realm
GMs can connect, the world locks to unpaused 1× until every observer disconnects, and only POV interaction is
permitted. `/pov stop` returns the camera but does not unlock acceleration or movement. See
[the observatory interface](../../apps/observatory/docs/INTERFACE.md#native-gm-pov).

## Files and integration

- `src/server/scripts/Custom/reforged_pov.cpp`: command, world-update and logout scripts.
- `src/server/scripts/Custom/custom_script_loader.cpp`: registers `AddSC_reforged_pov()`.
- `doc/reforged-pov/addon/ReforgedPOV/`: companion client addon and mocked Lua behavior test.

On the server, integrate this branch and enable the Custom script module in the existing build
configuration. The current scripts CMake code collects sources from enabled script directories;
the new C++ file is explicitly tracked despite the default Custom-directory ignore rule.
Preserve other custom script registrations when resolving loader changes. No SQL migration is needed.
Compile and validate on the server side before installing and restarting worldserver.

Copy the supplied `ReforgedPOV` addon folder into the observer client's `Interface/AddOns`, restart
the client and enable the addon. Log in with a dedicated GM observer and type `/pov`.
Select a character, use **Choose player** to switch, and **Stop watching** or `/pov stop` to return.
`/pov Charactername` selects directly. The HUD shows the latest successful bot action and three recent
actions, their age, and counts for consecutive repeats. Replace an existing addon and use `/reload`
to load version 0.2.0. Actions begin arriving after watching; human players have no AI action stream.

## Commands and requirements

| Command | Purpose |
| --- | --- |
| `.pov list [page]` | Send one alphabetical page of up to 50 eligible online characters; pages start at zero. |
| `.pov watch Name` | Travel to the character and start observing. |
| `.pov stop` | Detach the view, return to the starting location and restore observer settings. |

Commands use the existing `gm` permission. Observation also requires GM security and the `appear`
permission. Higher-security characters and other spectators are excluded. The observer must start
alive, ungrouped, out of combat, in the open world, without mounts, pets, vehicles, transports,
battleground queues or an existing remote camera. Use a dedicated observer: the existing GM
teleport command can bind the observer to dungeon instances.

The extension temporarily enables GM mode, invisibility and spectator state. It retains the original
location, phase and GM settings for stop/logout cleanup. Movement follows the selected character;
HUD snapshots are sent every 250 ms. Characters are referenced by GUID between updates.

## Addon protocol

The server sends addon whispers using prefix `RPOV`. Payload fields are separated by `|`.
The companion addon accepts server-origin whispers and rejects other player senders.

| Payload | Fields after the message type |
| --- | --- |
| `BEGIN` | page |
| `PLAYER` | name, level, class ID, zone ID |
| `END` | more pages (`0` or `1`) |
| `WATCH` | character name |
| `STATE` | name, health, max health, power type, power, max power, target name, target health, target max health, spell ID, remaining cast milliseconds, total cast milliseconds, zone ID, activity |
| `ACTION` | bot name, successful AI action name (maximum 120 bytes); emitted by the module patch |
| `ERROR` | explanation |
| `STOP` | none |

Channels report their remaining time with total duration zero; the public spell API does not expose
the hasted channel duration. The client therefore shows a countdown rather than a fabricated duration.

## Limits and server-side acceptance checks

This reconstructs health, resources, target, casts and activity. It does not mirror the player's exact
camera input, cursor, addon layout, action bars, inventory, quests or NPC/loot windows. Visible world
effects come from native rendering; snapshots are not a complete interaction log. AI action entries describe successful engine operations, including movement and interaction tasks;
they are not a complete spell/combat log. Unpatched modules do not send action entries.

Before treating this as ready to deploy, validate on the actual server with two clients and a bot:

1. Compile with Custom scripts enabled and verify command registration and addon packet delivery.
2. Compare health, resources, target, normal casts and channeled spells with the observed client.
3. Reopen the picker during updates, switch characters and stop. Verify original location, UI and GM settings.
4. Follow movement, zone/continent changes, vehicles, transports, dungeons and battlegrounds. Verify the
   correct instance or an explicit failure; entry to a different copy of the same dungeon requires particular care.
5. Disconnect the target, reload the observer UI and log out while watching or transferring. Check cleanup
   and persistence, including the fallback `.recall` path if return travel fails.
6. Verify normal accounts cannot use commands, higher-security targets remain unavailable, and the
   observer cannot affect encounters. Confirm bots continue acting independently.

Run `tests/test_addon.lua` from the companion addon directory using a Lua 5.1-compatible runtime.
It checks selection, HUD updates, sender validation, switching UI, stop/restore, reload recovery,
invalid input, and stale/missing server responses. Mocked tests do not validate WoW rendering or packets.
