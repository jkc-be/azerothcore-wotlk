# Python bot API

This module lets Python control headless characters owned by mod-playerbots. Network requests enter a
bounded queue; gameplay operations run from `WorldScript::OnUpdate`, after this checkout's map workers finish.
Each action receives its own acknowledgment and a subsequent observation. The existing bot provider continues to
own character creation, sessions, persistence, teleport handshakes and logout.

The Python package supplies a shared `Action`, `Observation`, `Transition` and `BotEnvironment` contract. Another
backend can implement that contract and use the same feature encoding and event accounting. No simulator or reward
policy is bundled. This is real-time control: `wait_ticks` waits for world updates and does not freeze or accelerate
the server. See [PROTOCOL.md](PROTOCOL.md) for timing and failure semantics.

## Server integration

The pinned Playerbots submodule includes the external-control integration. Initialize it with
`git submodule update --init --recursive` on a clean checkout before building.

1. Copy this directory beside the server's existing `modules/mod-playerbots/`.
2. For a different Playerbots checkout, follow the [integration patch instructions](integrations/playerbots/README.md).
   Do not reapply the patch to this fork's pin. Playerbots keeps its existing login/session lifecycle.
3. On the server, build both modules statically using its usual Playerbots-core build procedure. The standard module
   discovery collects these sources and `Addmod_python_apiScripts`; this handoff does not modify core code or SQL.
4. Add to the effective server configuration and log the named bots in using your usual Playerbots commands:

   ```ini
   PythonAPI.Enable = 1
   PythonAPI.Port = 5001
   PythonAPI.Playerbots.Names = "Bota,Botb"
   ```

   Options are read at startup. The distributed defaults leave the API disabled and expose no bots.

The listener binds **127.0.0.1 only** and accepts one controller connection, which can control multiple bots
sequentially. It has no authentication between local processes. If Python runs on another machine, use an existing
authenticated tunnel to the server's loopback port. A second controller is disconnected rather than sharing control.

## Playerbots ownership and reset

The adapter automatically discovers up to 64 configured bots at world-update boundaries. Human players and
selfbots are excluded. Registration alone leaves normal AI running; Python claims the bot before issuing actions.
The Playerbots patch suspends its ordinary decisions, command queues, delayed AI callbacks and per-bot random
scheduling while Python owns it. Session processing and Playerbots' teleport acknowledgments continue normally.

On release or disconnect, normal decisions resume. On logout or AI replacement, the binding becomes invalid before
any further wrapper mutation. A per-AI generation distinguishes a replacement from the old bot, even with the same
character GUID. A later login is registered as a fresh observation baseline.

`reset()` restores the claimed character to its position at claim time: stop combat/casting/movement, resurrect if
necessary, clear player auras/cooldowns, refill health/current power, and teleport back. It confirms success only
when Playerbots has completed the teleport and the character is alive and in world at the origin. This supports
outdoor maps, excluding taxi, vehicle, transport and instanced reset scenarios. It does not rewind inventory,
quests, XP, level, pet state, loot or the surrounding world. See the
[integration notes](integrations/playerbots/README.md) for patch application, lifecycle details and server verification.

The generic [PythonBotAPI.h](src/PythonBotAPI.h) contract remains available for additional providers. Its optional
`isAvailable` callback validates provider identity before actions, observations and release-side gameplay changes.

## Python use

The package requires Python 3.10+ and has no runtime dependencies. Install the `python/` package in your Python
environment, or put that directory on `PYTHONPATH`. Then:

```python
from acore_api import Action, Client, LiveEnvironment, SPELL_CAST_OK

with Client(host="127.0.0.1", port=5001) as client:
    bots = client.list_bots()
    # Select a configured bot explicitly; do not assume list order.
    bot_guid = next(bot["guid"] for bot in bots if bot["name"] == "Bota")
    with LiveEnvironment(client, bot_guid) as env:
        state = env.reset()  # Playerbots character reset.
        result = env.step(Action("observe"), wait_ticks=4)
        features = result.observation.features()
        print(result.events, features)
        # Move only to a known valid nearby destination on the current map:
        # result = env.step(Action.move_to(x, y, z), wait_ticks=4)
        # Cast a learned spell at an explicit raw GUID (use bot_guid for a self-target):
        # result = env.step(Action.cast(spell_id, target_guid), wait_ticks=4)
        # if result.spell_result != SPELL_CAST_OK:
        #     handle_cast_rejection(result.spell_result)
```

`LiveEnvironment` claims the bot on creation and releases it on clean context exit. `reset()` replaces the event
baseline only after an acknowledged new episode. `step()` returns the observation, event deltas and spell result;
the caller chooses rewards and any Gymnasium adapter. `terminated` reflects the bot's death. Timeouts raise
`OutcomeUnknown`, close the connection and **never automatically replay an action**. Reconnect, reclaim and inspect
or reset before proceeding. There is no cross-connection exactly-once guarantee.

`SPELL_CAST_OK` is 255 in this core, not zero. A cast result reports spell initiation/checking; it does not promise
that the spell has finished or landed. `casting` and subsequent game state describe later progress. Movement is a
pathfinding request limited to 50 yards; acknowledgment does not guarantee arrival or a valid complete path.

All `LiveEnvironment` instances sharing a `Client` serialize requests. Do not use multiple environments for the same
bot or concurrently mutate one environment. This is not a simultaneous multi-agent batch-step implementation.

## Browser visualization

`acore_api.ObservatoryPublisher` publishes existing controller observations to the browser Observatory without
opening another API connection or changing bot ownership. It provides a read-only map, searchable population,
charts, history and per-bot inspection. See the
[dashboard setup and synthetic preview](../../apps/observatory/docs/PYTHON.md) for controller integration and startup.

## Verification

Local transport/model tests use socket pairs and fake replies, with no game server or database:

```sh
cd modules/mod-python-api/python
python3 -m unittest discover -s tests -v
```

Integration validation: the full static server/module build, CTest, all 18 Python tests, the module C++ style check
and file-format checks passed. The SQL style check uses `origin/main` and passed. The full core C++ style check
reports existing violations. Enabled Python-control gameplay scenarios below still need validation.

On the separate server, validate these integration scenarios before using training results:

1. With no registrations, list is empty and an arbitrary player GUID is rejected. Human players remain unaffected.
2. Register two bots, claim both through one client, and verify normal AI stops only for those bots. Release one and
   verify its normal AI resumes. Reject a second controller connection.
3. Observe, move, and cast learned/unlearned spells, including invalid targets and unreachable destinations. Confirm
   each response's ID, actual world tick, spell result and position; check behavior rather than only acknowledgments.
4. Kill mobs and let the bot die between observations. Verify cumulative totals yield one event delta per event.
   Check release during casting/combat against the provider's intended behavior.
5. Reset a dead bot and test same-map and cross-map resets. Confirm the response occurs only after the provider
   finishes the handshake and the bot is alive and present on its destination map.
6. Disconnect while a command/reset is pending, let a controller idle for 30 seconds, and unregister/re-register a
   GUID. Check AI restoration, pending-request cancellation and absence of responses attributed to replacement bots.
7. Run with the server's intended map worker count. Confirm every provider lifecycle handoff occurs after workers
   finish, and that shutdown joins the socket thread before the bot provider is destroyed.

See [SOURCES.md](SOURCES.md) for the examples that informed the design and attribution for future commits.
