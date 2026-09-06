# Observatory interface v1

All live coordinates come from `ObjectAccessor::GetPlayers()` after `World::Update()` has joined map workers.
No telemetry query uses character save positions. World updates publish at most four snapshots per real second.
The writer sees serialized values only; it never reads a gameplay object. UTF-8 JSON numbers use world yards.

The world process creates a new private spool directory. A writer thread maintains:

| File | Meaning |
| --- | --- |
| `manifest.json` | Schema, run identity, 10 ms step, target count, loaded gameplay configuration |
| `initial.json` | First published snapshot with the complete cohort in the world |
| `latest.json` | Atomically replaced authoritative snapshot |
| `snapshots.ndjson` | Timestamped progression/population snapshots for export |
| `events.ndjson` | Ordered progression, convenience, and optional diagnostic events |
| `control.txt` | Private atomic mailbox: `run sequence speed paused` |

Snapshots include `run`, monotonically increasing `seq`, `simMs`, `realMs`, `readyAtMs`, `requestedSpeed`,
`achievedSpeed`, `paused`, `baseline`, `completed`, `controlSeq`, `backlogMs`, `maxTickUs`, `overloaded`,
`ready`, `expectedBots`, `onlineBots`, `activeBots`, `fault`, and `bots`.
`achievedSpeed` is the simulated/real delta since the previous sample, including pauses; it is not the requested rate.
`maxTickUs` measures actual world-update CPU/wall cost in the sample interval, excluding snapshot serialization.
`activeBots` counts identified online bots with an AI update within 10 simulated seconds. Natural AI decision delays,
casts and deaths do not disable simulation. Compare each bot's `aiUpdates` counter to detect a stalled path.
Bots transferring between maps remain in `onlineBots`, but are temporarily absent from position snapshots.
`overloaded` means more than one simulated second of outstanding steps. Debt is never discarded in virtual mode.

Each bot has stable string `id`, `name`, `map`, `instance`, `zone`, `x`, `y`, `z`, `level`, current `xp`,
`nextLevelXp`, `health`, `maxHealth`, `money`, cumulative `earnedXp`, `deaths`, `questCompletions`,
`activity`, `lastAiMs`, `aiUpdates`, equipped item entries in `gear`, and quest-log IDs, states, creature/object
counters (`objectives`) and item counters (`items`).
The enclosing snapshot supplies the run and simulation timestamp for every bot. Map and zone selectors use server IDs;
the map is a coordinate plot without proprietary map tiles. Different instances share the same coordinate plane;
the selected-bot detail identifies the instance.

Events contain run, global event `seq`, `simMs`, bot/actor GUID, kind, value, detail/context and map/instance.
Progression events: `xp`, `quest_reward`, `death`, `level_change_attempt`, `teleport_attempt`.
`xp` counts accepted core XP, including standard bonuses; module direct XP field mutations remain separate.
`bot_action` identifies completed AI actions. `shortcut` identifies factory operations and explicit bot resource,
level, resurrection and XP mutations. A mutation event records an attempted explicit setting, not necessarily a grant;
for example, restoring a temporary shopping balance is also labelled. Factory and AI context names accompany nested
core events. Initial equipment and generated levels are baseline state, never reconstructed as earned XP.
Teleport attempts are not successful travel distance. Standard taxis, hearthstones and transports remain gameplay.

`Observatory.Trace = 1` adds per-step positions, melee swings (including misses), aura ticks, damage inputs/final damage,
cast start/finish/cancel, cooldown durations, resource changes, and deaths/respawns of creatures interacting with bots.
Use these records for small timing comparisons, not the default throughput benchmark. Damage records report core damage
values; lethal overkill is possible. Health/resource setters are distinct from damage and identify before/new health.
Trace creature GUIDs can change for dynamic respawns; assess those using spawn identity/server logs instead of assuming
that every respawn can be paired by GUID. `analyze.py` reports the pairs it can observe and does not declare correctness.

The local Python adapter exposes the following HTTP endpoints. It binds only to `127.0.0.1`; use a tunnel for a remote
browser. All API endpoints require `Authorization: Bearer TOKEN`; static UI files do not. No CORS is enabled.

| Request | Response |
| --- | --- |
| `GET /api/stream` | SSE `data: SNAPSHOT` records; real-time heartbeats; no replay on reconnect |
| `GET /api/snapshot` | Latest snapshot or 503 while starting |
| `GET /api/events` | At most 500 recent events from a bounded journal tail |
| `GET /api/export/NAME` | Manifest, initial state, snapshots, or event journal; fixed cutoff at request start |
| `POST /api/control` | JSON `{"run":"…","speed":10,"paused":false}` → 202 accepted sequence |

A 202 response is mailbox acceptance. Application is acknowledged by a later snapshot's `controlSeq`, speed and pause
state. Controls are validated again by the writer, applied by the world thread between joined updates, and scoped to
the run. The mailbox intentionally coalesces rapid requests: a newer sequence supersedes an unapplied older request.
Baseline mode permits 1× without pause only. Completed/faulted runs cannot be resumed by the adapter.

Every SSE viewer has an independent connection and a five-second socket deadline. At most 16 streams are accepted.
Slow viewers skip snapshots and reconnect to the newest state; browser rendering never drives simulation ticks.
The writer's latest-snapshot slot also coalesces during slow disk I/O; sequence gaps expose this. Events are retained
in a bounded 65,536-record queue. Overflow or writer failure invalidates and freezes the run (`journal_failure`),
never silently yielding a trustworthy-looking incomplete run. A full bot-operation queue similarly freezes the run
with `bot_operation_queue_overflow`; its dropped operation makes that run invalid. If the disk itself fails, the last
visible snapshot may remain unchanged: the UI reports it stale after three seconds. Exports taken during a write may end in one partial
NDJSON line; export after shutdown for a complete final journal.
