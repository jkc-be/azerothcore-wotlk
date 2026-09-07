# Alles Ollama pilot

A Go worker interprets the configured characters' bounded perceptions and private memory context.
Worldserver owns leases, evidence validation, memory changes and speech. The worker has no database
connection or control of bot movement. The existing playerbot AI continues playing normally.

Build and test with Go 1.26 or newer:

```sh
cd apps/alles
go test -race ./...
go build -o ../../build/alles-interpreter ./cmd/alles-interpreter
```

Configure an explicitly selected, already installed model (no automatic downloads or fallback model):

```json
{
  "base_url": "http://fedora-gpu:11434/v1",
  "model": "qwen3:8b-q8_0",
  "bridge": "127.0.0.1:8779",
  "token_file": "/private/alles/bridge-token"
}
```

Create a random private token of at least 32 characters. The worker lists available models before
connecting. It uses non-streaming Chat Completions with JSON schema output, a 20-second timeout,
1024 output tokens, and no reasoning. Complete model input is conservatively capped at 12 KiB of
UTF-8 plus output allowance for the measured 16,384-token deployment context. Oversized jobs fail
closed and retain the normal fallback deadline. Recheck model context before changing the profile.

Run `alles-interpreter --config FILE --fingerprint`, then set the matching `Alles.Worker.Profile`,
`Alles.Worker.Model`, `Alles.Worker.TokenFile`, and `Alles.Interpreter.TrialLedger` in `alles.conf`.
Select `Alles.Worker.Mode = "bridge"`. Configuration is fixed for a world boot. Changing the model,
prompt contract or worker profile requires a new recorded trial and matching world configuration.
The default `inprocess-fake` remains available for deterministic C++ tests.

The external pilot has **one fleet slot and one HTTP attempt per job**. Connections are authenticated
loopback JSON lines, bounded to four sockets, 64 KiB per frame and bounded mailboxes. Responses carry
request IDs and can arrive out of order. Workers renew 15-second leases independently of HTTP work.
Lost leases, expired permits and previous-boot tokens cannot change memory. A pre-attempt release
can reassign a job without resetting admission time; provider failures fall back without retrying.

Each permit is appended and synced by an I/O thread before authorization. Pending and uncertain
attempts remain charged. The private ledger has a process lock and a pinned profile; reopening it
preserves the trial's request count. The default trial limit is 100, with no reset on worker or world
restart. At exhaustion the worker polls every five seconds but receives no new job or HTTP permit. The allowance
does not refill with time. Pending perceptions reach their admission deadline (45 real seconds or 120 game
seconds, whichever expires first) and are consumed through template fallback; they are not kept for later model
replay. Normal gameplay and memory retelling continue. Provider outcomes, token usage
and latency are logged to the world and worker logs; the ledger retains reservations. This pilot
has no paid-provider configuration, fleet expansion, token-rate governor, retries, or strict lab mode.
Those parts of the broader implementation plan remain separate work.

Offline fixtures use the same HTTP client and parser:

```sh
alles-interpreter --config FILE --fixture JOB.json --fixture-ledger EVALUATION.ndjson --max-requests 100
```

`--warmup` allows up to 120 seconds for fixture model loading. Every fixture call, including failures
and warmup, reserves from that separate evaluation ledger. Gameplay calls keep their 20-second bound.
Fixtures print model output for inspection; ordinary worker logs contain operational metadata only.

## Native server and dashboard

This checkout's `./server start`, `stop`, `restart`, `status` and `logs` include the
`acore-alles-interpreter` user service when bridge mode is enabled. The installed worker executable
runs without the Go toolchain. A worker-only restart needs no game restart; deploying C++ or changing
startup settings does. Stop cancels worker intake before requesting normal world shutdown and saves.

Set `Alles.Telemetry.Directory` to an existing private directory and point the existing Observatory
bridge's `--spool` at it. Worldserver samples live configured bots once per second after map workers
finish, then publishes immutable JSON through the I/O thread. The dashboard shows live positions,
health, equipment, bags, quests, memory counts and worker request counters. Its controls stay disabled
on an ordinary realm; simulation mode remains off. Samples older than ten seconds are explicitly stale.
Old simulation spools are retained independently.

The world also keeps the two Observatory journals beside `latest.json`. `events.ndjson` receives the
records the core's `Observatory::Event` tap raises for configured owners on an ordinary realm
(`bot_action`, `xp`, `death`, `quest_reward`, `shortcut`, `teleport_attempt`, …, in the Observatory record
shape) plus the module's own kinds: `alles_perception` (each admitted or dropped perception, described),
`alles_memory` (a memory formed, with its formation mode, or revised), `alles_said` (autonomous speech and
whether anyone was in range), `alles_owner` and `alles_save` (memory store state and save outcomes) and
`alles_worker` / `alles_request` (worker connection and charged requests). `snapshots.ndjson` receives every
published sample so a reconnecting browser restores recent history and the bridge keeps its long-term
tiers. Both rotate at `Alles.Telemetry.JournalSegmentBytes` (64 MiB by default) so the bridge prunes old
segments. The same tap gives each bot `aiUpdates`, `lastAiMs`, `actions`, `lastAction`, cumulative
`earnedXp`, `deaths` and `questCompletions`, and the snapshot `activeBots`, `runTotals`, `journal` and
`alles` (ingress) health plus the coordinator counters under `interpreter.stats`. Recording is best effort:
a full queue or a failing disk drops records and counts them; gameplay never waits on telemetry.

Every boot is a new run in the same directory. At startup the world moves the previous run's journals,
`latest.json`, `manifest.json` and the bridge's derived files into `archive/<run>/`; the bridge starts its
journal tiers afresh when the run id changes and ignores records of another run. The bridge exposes the
worker's own log tail (interpretation job and conversation turn outcomes, token usage, latency, connection
errors) at `/api/worker-log`; it reads `env/dist/logs/alles-interpreter.log` of its own checkout by default
and `--worker-log PATH` selects another file. The dashboard's Interpreter panel shows it beside the worker
status, the request budget, the conversation counters, the coordinator counters, the memory owners and the
alles event feed. Counters a world build does not report are shown as unmeasured, never as zero.

The dashboard's Memory region inspects the stores themselves. It lists every owner with a committed store
in `alles_actor`, shows the selected character's committed memories (the rendered sentence, kind, source and
attribution, confidence, salience, formation mode, formed and recalled times) with a filter and sort, its
pending perceptions, and the committed revision beside the live store's revision, since the table lags the
store by one save interval. "Talk to memory" asks the worker's own Ollama model to answer an observer's
question in the character's voice from those committed memories: an out-of-game interview that the world
never hears, that forms no memory or speech, and that does not draw on the interpreter budget (it does share
the GPU, so one question is answered at a time). The bridge reads the tables through the `mysql` client with
the `CharacterDatabaseInfo` of its checkout's `worldserver.conf` and finds the worker model in `worker.json`
beside `Alles.Worker.TokenFile`; `--world-conf`, `--worker-config`, `--mysql` and `--no-memory` override
that. See `apps/observatory/docs/INTERFACE.md` for the `/api/memory` endpoints.

## Natural conversation

Delivered replies are journaled as `alles_conversation`, including the actual spoken text and context naming
the human, SAY/YELL channel, proposed action, its server-validated result, and provider job. This works with
ambient speech disabled. Active-bot counts include only the current online cohort; lifetime progression
totals retain offline owners. The bridge detects new runs from the producer without needing a browser request.

With bridge mode and `Alles.Conversation.Enable=1`, actually heard, understood human SAY/YELL enters a separate
conversation queue. No HELP keyword or punctuation is required. Each nearby autonomous configured bot receives
its own context and may answer. A direct name at the start (optionally after a greeting) routes only to that bot;
the model also considers whether other phrasing addresses someone else. Bot speech never starts another
conversation job, so responding bots cannot generate an automatic reply loop.

Each bot gets the current player message, identity/class, place, up to eight recent utterances for that human/bot
pair, and up to four matching, deduplicated memories. Retrieval currently uses word overlap, not embeddings.
History expires after two idle minutes or logout and is not persisted as a separate conversation transcript.
Ordinary heard speech still follows the existing durable memory path. Disable `Alles.Speech.Enable` to stop
unsolicited memory recitals while leaving conversation enabled. Death memories are not a default conversational
subject. Model interpretation can still be imperfect; server validation bounds what an interpretation can do.

The response schema is `reply`, `text`, `action`. Text is limited to 255 UTF-8 bytes with no control characters
or client markup. Actions are `none`, `wave`, `follow`, `stop`, and `assist`; arbitrary commands are impossible.
Available schema choices follow the supplied current capabilities, and the server checks live state again:

- Follow temporarily pauses the independent bot's noncombat strategies and uses normal follow movement. Combat
  AI remains active. Stop restores the saved strategies; a two-minute lease, logout, death, map/phase change,
  distance over 100 yards or control handoff also ends the temporary follow. Another player cannot take over an
  active follow through this path. Temporary strategies are not written to the playerbot repository.
- Assist can engage only the exact visible nearby hostile NPC already fighting the requesting player when the
  job was created, if it is still eligible at delivery. It calls the existing playerbot attack action. It cannot
  select arbitrary targets, initiate PvP or attack a newly selected unrelated creature.
- Wave uses the normal emote. Healing, trade, quests, party changes and teleportation are not implemented actions.
  Failed action checks replace the proposed promise with a truthful failure line.

Every response is fenced by the current human and bot incarnations and rechecked for life, visibility, phase,
map and hearing range. Departed players do not receive stale replies. Responses are staggered at least one second
apart. There are at most 32 retained human turns (three pending turns per human), 32 pending model jobs and a
45-second model admission lifetime. Follow-ups to a busy human/bot pair wait in order. Overdue replies are
omitted, with a player-facing timeout message when the pair is still nearby; over-capacity intake is counted in
telemetry. This is bounded local conversation, not guaranteed delivery under unlimited spam.

Conversation jobs take priority over background memory jobs in the shared single GPU slot. For continuous local
play set `Alles.Interpreter.BudgetMode="rolling"` and `Alles.Interpreter.RequestsPerMinute=30` (range 1–120).
Reservations count against a trailing one-minute window and replenish as they age out; failures are not refunded.
Timestamps and cumulative request count survive a worker/world restart. The private ledger is compacted into a
synced checkpoint before 1 MiB, keeping the cumulative count and recent timestamps under a stable lock file.
Old trial ledgers remain valid in trial mode and do not reset themselves. A changed prompt/profile needs a new
recorded ledger. The dashboard and `./server status` distinguish rolling allowance from lifetime trial counts.

Offline conversation evaluation uses the same schema, client and separate durable fixture budget:

```sh
alles-interpreter --config FILE --conversation-fixture CHAT.json --fixture-ledger EVALUATION.ndjson --max-requests 20
```

The fixture is `{"jobToken":"fixture","permitId":"fixture","remainingMs":25000,"context":{...}}`;
context has `bot`, `player`, `message`, `selfContext`, `place`, `audience`, `history`, `relevantMemories`,
`inCombat`, `followingPlayer`, `canFollow`, `nearbyThreat`, and `capabilities`.
