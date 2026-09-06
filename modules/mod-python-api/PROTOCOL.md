# Protocol version 1

Transport is UTF-8 JSON, one object per newline. Requests are flat objects whose scalar values are encoded as
strings. Responses use real JSON booleans/numbers; GUIDs, identities, ticks, elapsed milliseconds and event counters
are decimal strings to preserve 64-bit values. The Python package handles these conversions.

One controller owns the connection; it may claim several explicitly registered bots. Requests must have strictly
increasing positive uint64 IDs within that connection. The server reads the next request after writing the previous
response. Clients should send only one outstanding request. There are no unsolicited snapshots or keepalives.

Limits: 4096 bytes per request including newline, 16 queued commands, 64 registered bots, and a 30-second server
deadline from the start of reading each request through its response. An idle controller must send an `observe` or
`list` request within 30 seconds to retain the connection. The Python client additionally applies an absolute
10-second request deadline by default, adjustable with `Client(timeout=...)`.

Malformed JSON, unknown/duplicate fields, invalid numbers, out-of-range ticks, unknown operations and non-increasing
IDs close the connection. Valid requests rejected by gameplay/ownership checks receive `ok: false` and an error code.
Slow clients cannot block the world loop; socket writes execute in the joined Asio thread. A disconnected client
loses its claims on the next world update after the transport detects the disconnect, at latest its deadline.

## Requests

Every request contains `version: "1"`, `id`, and `op`. All operations except `list` require `bot`, a raw player GUID.

| Operation | Additional fields | Behavior |
| --- | --- | --- |
| `list` | none | Registered GUIDs, names and whether each provider supports reset |
| `claim` | none | Ask the provider to suspend normal AI; return a subsequent observation |
| `release` | none | Stop wrapper movement, return control to the provider |
| `observe` | optional `wait_ticks` | Observe after the specified future world updates |
| `stop` | optional `wait_ticks` | Stop movement, then observe |
| `move_to` | `x`, `y`, `z`, optional `wait_ticks` | Path toward finite coordinates within 50 yards on this map |
| `cast` | `spell`, `target`, optional `wait_ticks` | Cast a learned spell at an explicit GUID, then observe |
| `reset` | none | Begin provider reset; wait for its completion and advance the episode |

`wait_ticks` is an integer from 1 through 100, default 1. `claim` also waits at least one future world update.

Example:

```json
{"version":"1","id":"7","op":"cast","bot":"42","spell":"585","target":"17379390962000000001","wait_ticks":"4"}
```

Actions execute at a world-update boundary. An observation is collected at a later boundary after at least
`wait_ticks` world updates; no snapshot buffered before the command can satisfy its response. This does not imply
one full map simulation update per world tick: AzerothCore schedules different map updates on different iterations.
The server continues running while Python thinks. `elapsed_ms` is the sum of actual world-update diffs since this
bridge started, not a virtual clock or fixed simulation time.

## Responses

Every response includes `version: 1`, the matching `id`, `ok`, `world_tick` and `elapsed_ms`. Successful list responses
include `bots`; successful releases have only the envelope. Other successful operations include `observation` and
`spell_result`. Cast failures from `CastSpell` appear in `spell_result` even when the request itself was processed
successfully. Non-cast operations use `SPELL_CAST_OK` (255).

An observation contains:

- `guid`, `name`, `registration`, `episode`.
- `health`, `max_health`, `power`, `max_power`, `power_type`, `level`.
- `map`, `instance`, `position: [x, y, z, orientation]`.
- `alive`, `combat`, `casting` as JSON booleans.
- `events: {kills, deaths, levels}` as cumulative uint64 decimal strings.

`kills` counts `OnPlayerCreatureKill` callbacks, not XP awards or party kill credit. `deaths` counts
`OnPlayerJustDied`; `levels` sums positive level increments. Counters start at registration, continue through resets,
and are never cleared on observation. Consumers subtract the previous totals within the same registration/episode.
They establish a fresh baseline after reset or reconnection. XP/money are deliberately not inferred from pre-award
hooks or inventory changes.

Possible semantic errors: `bot_not_registered`, `bot_unregistered`, `bot_unavailable`, `claim_required`,
`invalid_destination`, `invalid_spell_or_target`, `reset_refused`, `reset_left_bot_dead`, `request_pending`.

The episode counter advances only after the provider reports reset completion and the bot is alive, in world and
not teleporting. A provider may choose a character-only reset or a complete scenario reset; clients must know which.
Registration identities and counters are process-local and restart with the server. A new connection establishes
new baselines; do not merge its event totals with an earlier connection without an application-level run identity.

An EOF or timeout after sending an action leaves its outcome unknown. The command may already have executed even
if its acknowledgment did not arrive. The server drops queued commands from inactive connections, but cannot undo
an operation already applied. Repeated IDs on a connection are rejected; reconnecting does not deduplicate actions.
