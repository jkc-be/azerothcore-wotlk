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
| `NAME.NNNNNN.ndjson` | Rotated journal segments when `Observatory.JournalSegmentBytes` is set (see retention below) |
| `control.txt` | Private atomic mailbox: `run sequence speed paused bots` |

The bridge adds its own long-term files beside them: `rollup.ndjson` (snapshot summary buckets), `milestones.ndjson`
(complete snapshots at fixed simulated times), `events-rollup.ndjson` (per-kind record counts per bucket) and
`progression.ndjson` (every non-trace event record). See [journal retention](#journal-retention).

Snapshots include `run`, monotonically increasing `seq`, `simMs`, `realMs`, `readyAtMs`, `requestedSpeed`,
`achievedSpeed`, `paused`, `baseline`, `completed`, `controlSeq`, `backlogMs`, `maxTickUs`, `overloaded`,
`ready`, `expectedBots`, `maxBots`, `populationPending`, `runTotals`, `onlineBots`, `activeBots`, `fault`, and `bots`.
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
optional local client artwork is calibrated from WorldMapArea and WorldMapOverlay DBC data.
Different instances share the same coordinate plane;
the selected-bot detail identifies the instance.

Events contain run, global event `seq`, `simMs`, bot/actor GUID, kind, value, detail/context and map/instance.
Progression events: `xp`, `quest_reward`, `death`, `level_change_attempt`, `teleport_attempt`.
`xp` counts accepted core XP, including standard bonuses; module direct XP field mutations remain separate.
`bot_action` identifies completed AI actions. `shortcut` identifies factory operations and explicit bot resource,
level, resurrection and XP mutations. A mutation event records an attempted explicit setting, not necessarily a grant;
for example, restoring a temporary shopping balance is also labelled. Factory and AI context names accompany nested
core events. Initial equipment and generated levels are baseline state, never reconstructed as earned XP.
Teleport attempts are not successful travel distance. Standard taxis, hearthstones and transports remain gameplay.

With `AiPlayerbot.AutoDestroyJunk = 1`, autonomous random bots discard the cheapest unneeded grey-quality stack
when normal bags exceed 80% occupancy. Each `junk_discarded` event records the stack count in `value` and item ID in
`detail`. Equipped/banked items, quest requirements (including completed quests awaiting turn-in), quest starters,
explicitly retained loot, upgrades and useful supplies are protected. The option defaults off in the module and
excludes player-controlled bots. It is separate from the older, broader `smart destroy item` maintenance action.
Looting checks actual storage capacity, including existing stack space. A source with items that cannot fit is
deferred for 30 simulated seconds and emits `loot_inventory_blocked`, with the source GUID in `detail`, so an
inventory-full gathering loop does not keep taking priority over other activities. No useful items are deleted
when the bot has no discardable junk; vendor/bag acquisition behavior remains necessary for that case.

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
| `GET /api/events?scope=progression` | At most 2,000 recent non-trace events retained by the journal follower |
| `GET /api/event-stats` | Per-kind record counts since the bridge started following, plus counts for the last minute |
| `GET /api/event-stats?scope=long-term` | `{bucketMs, points}`: per-kind record counts per bucket, whole run |
| `GET /api/history` | At most 1,000 complete snapshots from the most recent 4 MiB of the snapshot journal |
| `GET /api/history?scope=long-term` | `{bucketMs, points}`: summary buckets, whole run; `limit` folds older ones |
| `GET /api/retention` | Journal sizes, segments, pruning, rotation availability and long-term tier counts |
| `GET /api/export/NAME` | Manifest, initial state, a journal (segments then live file) or a long-term file |
| `POST /api/control` | JSON `{"run":"…","speed":10,"paused":false,"bots":25,"observerMode":1}` → 202 accepted sequence |

A 202 response is mailbox acceptance. Application is acknowledged by a later snapshot's `controlSeq`, speed and pause
state. Controls are validated again by the writer, applied by the world thread between joined updates, and scoped to
the run. The mailbox intentionally coalesces rapid requests: a newer sequence supersedes an unapplied older request.
Baseline mode permits 1× without pause only. Completed/faulted runs cannot be resumed by the adapter.

## Adaptive Max speed

`POST /api/control` also accepts `"speed":"max"` with optional `"backlogLimitMs":100` (integer, 10–60,000).
World snapshots advertise `"speedStep":0.1` when decimal controls are supported. Max then selects from 1× to 10×
in 0.1× steps (including 2.1× and 3.2×); numeric API requests accept that same range and precision. Without this
capability, Max retains the legacy 1×, 2×, 5× and 10× rates and rejects unsupported numeric requests before writing
the mailbox. Manual dashboard buttons remain shortcuts to those four speeds.
The backlog limit is outstanding **simulated** milliseconds,
not a wall-clock delay. Selecting a numeric speed returns to manual control. Pause/resume and population changes can
retain Max by sending `"speed":"max"` again. The default backlog target is 100 ms; the dashboard allows editing it.

The bridge runs the controller every 250 ms, independently of browser connections. Max starts at 1×, waits for world
acknowledgement and a settled population, then measures three seconds of backlog using one-second medians.
It steps down only when backlog grows across all three windows and the latest median exceeds the target.
Brief spikes and flat backlog do not trigger throttling. Max probes one faster step after five seconds without
sustained growth; there is no additional retry cooldown. Falling backlog above the target is allowed to drain before
probing again. Measurements restart after speed changes, pause, stale telemetry or pending acknowledgements.
The limit is a feedback target, not a hard cap: the trend window, mailbox latency and load spikes can exceed it.
If debt keeps accumulating at 1×, Max reports that condition; it never discards debt or pauses gameplay automatically.
On a server supporting decimals, sustained overload can reduce speed by more than 0.1×: backlog growth estimates
available capacity, rounded down with 0.1× headroom to clear debt. This avoids a long series of tiny reductions after
a large capacity loss. Legacy servers still reduce by one preset.

The mailbox speed field accepts decimal text on updated servers. Pacing accumulates integer tenths of a microsecond,
preserving the remainder across speed changes and pause; gameplay continues to consume ordinary 10 ms steps.
Decimal support requires rebuilding and installing worldserver. Updating only the bridge cannot enable it in an
already running legacy simulation.

HTTP/SSE snapshots add `speedControl: {mode, backlogLimitMs, status}` from the bridge. `requestedSpeed`, `backlogMs`
and `controlSeq` remain the world's authoritative values; the private mailbox format and world journal are unchanged.
Max waits on stale telemetry or pending acknowledgements. A GM observer, run change, completion, fault, rejected control
or another controller's mailbox request cancels Max. Re-enable it explicitly after GM POV ends. Baseline and read-only
feeds reject Max. Restarting the bridge returns to manual mode at the world's last requested speed; reconnecting or
closing the browser preserves Max while the bridge remains running.

The bridge follows `events.ndjson` incrementally from its tail: it reads only appended bytes, keeps the newest 500
records of every kind and the newest 2,000 progression records, and counts records per kind. With `Observatory.Trace`
enabled, per-step trace kinds (`position`, swings, ticks, casts, cooldowns, resource setters, creature deaths) dominate
the bounded file tail, so the dashboard reads the progression scope for its live feed and inspector while the event
mix table shows the trace volume. The seed batch read at startup restores the display but is not counted. Counts reset
when the bridge restarts; they continue across journal rotation.

### Journal retention

Each journal has two tiers. The **recent** tier is the raw record: the live journal plus its rotated segments on disk,
and the bounded deques above in memory. The **long-term** tier folds every record into fixed buckets of `BUCKET_MS`
(five simulated minutes) that are kept for the whole run, appended to the bridge's own files as each bucket closes and
reloaded when the bridge restarts:

| Stream | Long-term file | Contents |
| --- | --- | --- |
| Snapshots | `rollup.ndjson` | One point per bucket with the dashboard summary keys (see below) |
| Snapshots | `milestones.ndjson` | The first complete snapshot after every `MILESTONE_MS` (ten minutes) boundary |
| Events | `events-rollup.ndjson` | Per-kind record counts per bucket (`kinds`, `samples`) |
| Events | `progression.ndjson` | Every non-trace record, deduplicated by `seq` across bridge restarts |

In a rollup point, counters and distributions (`xp`, `quests`, `deaths`, `money`, `questsActive`, `levels`, `zones`)
keep the newest value, gauges (`meanLevel`, `meanHealth`, speeds, `backlogMs`, population, `activity`, `pausedShare`)
are sample-weighted means, `minLevel`/`maxLevel` keep their extremes and `tickMs` its maximum.
Long-term responses fold consecutive buckets together beyond `limit` points (default 720, at most 5,000);
`buckets` on a point counts how many were folded and `samples` how many records or snapshots it holds. The open
bucket is included with `partial: true`.
Seed snapshots enter the rollup unless it already holds their `seq`; seed events feed `progression.ndjson` but not
the counts. Snapshots keep the newest 20,000 buckets in memory; the files keep every bucket.

With `Observatory.JournalSegmentBytes` set (at least 1 MiB), the world renames `snapshots.ndjson` and `events.ndjson`
to `NAME.NNNNNN.ndjson` once a flushed batch takes them past that size and continues in a fresh file. The bridge holds
each journal's descriptor, drains a renamed segment to its end before reading the successor from its start, then
deletes the oldest segments it has consumed while their total exceeds `--retain-bytes` (default 2 GiB per journal).
The live file is never deleted, so a world without rotation keeps growing; `/api/retention` reports `rotation: false`
in that case and the dashboard's Journal panel says so. Exports of a rotated journal stream the retained segments
followed by the live file; older records survive only in the long-term files.

Every SSE viewer has an independent connection and a five-second socket deadline. At most 16 streams are accepted.
Slow viewers skip snapshots and reconnect to the newest state; browser rendering never drives simulation ticks.
The writer's latest-snapshot slot also coalesces during slow disk I/O; sequence gaps expose this. Events are retained
in a bounded 65,536-record queue. Overflow or writer failure invalidates and freezes the run (`journal_failure`),
never silently yielding a trustworthy-looking incomplete run. A full bot-operation queue similarly freezes the run
with `bot_operation_queue_overflow`; its dropped operation makes that run invalid. If the disk itself fails, the last
visible snapshot may remain unchanged: the UI reports it stale after three seconds. Exports taken during a write may end in one partial
NDJSON line; export after shutdown for a complete final journal.

Optional map endpoints also require the bearer token: `GET /api/maps` returns the extracted area catalog, and
`GET /api/maps/<numeric-tile-name>.png` returns local artwork. The bridge reads these from `--maps`; no client archive
is exposed. Map assets are decoded and cached by the browser independently of simulation updates. See `MAPS.md`.

## Population controls

`bots` is optional for existing HTTP clients. It is an integer from zero through `maxBots` (at most 100).
Omitting it preserves the latest accepted target, including a target still awaiting world acknowledgement.
The bridge rejects unknown keys, invalid types, out-of-pool targets and population changes in baseline mode.
An old worldserver without `maxBots` keeps speed/pause compatibility, but the population field is disabled.
The updated world requires the five-field private mailbox; update the bridge together with the core and module.

`expectedBots` is the requested online population; `populationPending` stays true until the module has no pending
logins, its roster matches, and all requested bots are in the world. HTTP acceptance and `controlSeq` acknowledge the
request, not successful logins. While paused the target can change, but login/logout work waits for Resume.
`ready` and `readyAtMs` retain the first complete population's run-start milestone. Benchmarks must also require
`populationPending == false` and keep the target fixed throughout their comparison window.

The startup `AiPlayerbot.MinRandomBots` and `MaxRandomBots` must be equal and between 1 and 100; these provision the
eligible pool. `Observatory.BotCount` sets the initial online target, including zero. Optional `BotGuids` now limits
eligibility to a pool whose size is at least the initial target and no larger than MaxRandomBots. Its size further
caps the browser control. Empty BotGuids uses eligible random-bot accounts prepared by the normal module factory.
No human or add-class account is selected. No accounts or characters are deleted when shrinking.

The module uses normal asynchronous login and save/logout paths. Surplus GUIDs are removed in descending order,
waiting for pending logins first. Missing online bots are reconciled by the normal random manager; login failures
remain visible. A mismatch lasting ten simulated minutes freezes the run with `population_timeout`, rather than
claiming that the requested population is active. This timer does not advance while paused.

The journal records `population_target` (value = target, detail = control sequence), `population_logout` on each
removed bot, and sampled `population_join`/`population_leave` (detail = GUID). Run-level records have empty `bot`;
new arrivals' level, gear and quest state are retained in snapshots. These population events identify operator
changes and automatic recovery; compare event times with snapshots and the effective initial settings.
`runTotals` contains cumulative `xp`, `quests`, and `deaths` across all bots seen during the run, including departed
bots. The charts use these totals, so shrinking does not erase earned progression. Distribution charts still
reflect the currently online population. Logout/rejoin does not reset the per-bot in-memory run counters.

## Native GM POV

`Observatory.AllowGmObservers = 1` admits authenticated realm GMs (security 2 or above) only when the
Custom `reforged_pov` script is loaded. Default is 0; ordinary accounts remain rejected at any speed.
Baseline mode refuses this option. No second world or realm is required.

Authentication acquires an observer lease and requests unpaused 1× before character login. It lasts until the
world session is destroyed, including character selection, `/pov stop`, and disconnect cleanup. Observers bypass
the normal one-minute offline reconnect grace period and clean up on the next world session update. The last departure
leaves 1× selected; the operator can then accelerate or pause again. Accumulated simulation debt is retained,
but catch-up bursts are suppressed while an observer is connected. Requested 1× can still fall short under load.

Snapshots add `observersAllowed` (effective support), `observers` (connected leases), and `controlError`.
The bridge rejects acceleration/pause with HTTP 400 while `observers > 0`. If a connection races an already
accepted mailbox request, the world consumes its sequence without applying it and publishes `controlError`.
Clients must check the resulting state and error as well as `controlSeq`. Population changes at unpaused 1× remain
available. `observer_connect` and `observer_disconnect` journal events put the account ID in `value` (not a bot GUID).

Observers enter invisible GM spectator mode. What else they may do is the run-level **observer mode**, set initially by
`Observatory.ObserverMode` and changed live with `"observerMode"` in `POST /api/control` (the dashboard's Locked / Roam /
Full GM buttons). It is applied automatically to every observer at login and to connected observers on the next POV
update; it is independent of the 1× speed lock and may change while GMs are connected. Snapshots report it as
`observerMode`; the mailbox gains a sixth field that older worlds ignore and older bridges omit. Each applied change
journals `observer_mode` (value = mode, detail = control sequence).

| Mode | Behaviour |
| --- | --- |
| 0 locked (default) | Client movement disabled. A core opcode allowlist permits character creation/login, session bookkeeping, read-only queries, selection, POV chat and server teleport ACKs. Only `.pov` through SAY chat; movement, casts, attacks, loot and other GM commands are blocked |
| 1 roam | The observer's own movement opcodes and their acknowledgements are also accepted. Commands stay `.pov` only. The observer's position now affects grid load and visibility workload |
| 2 full GM | The opcode allowlist and chat filter are bypassed: any GM command and action is permitted. Every observer command is journaled as `observer_command` (value = account ID, detail = text). Such a run mutates the disposable databases and is not a clean comparison |

Install the addon described in `doc/reforged-pov/README.md` to use `/pov` in the client. While `/pov watch` binds the
observer's sight to a bot, movement is disabled in every mode; `/pov stop` restores it according to the current mode.
The observer can load extra grids and affect visibility/workload: disable admission during scientific comparisons
and benchmarks. These remain disposable databases: provision observer accounts/characters in the clean fixture
if they must survive a future run reset. Never resume virtual-time database output to retain an observer character.
