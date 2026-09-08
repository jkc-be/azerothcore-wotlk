# Alles agent worker

A Go worker interprets the configured characters' bounded perceptions and private memory context.
Worldserver owns leases, evidence validation, memory changes and speech. The worker has no database
connection or control of bot movement. The existing playerbot AI continues playing normally.

See [the runtime contract](RUNTIME.md) for actor scheduling, live policy controls, shared interviews,
the optional native AI SDK adapter, profile migration and the remaining live acceptance gates.

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

The version-2 worker negotiates concurrency; provisional defaults allow one job/call across the fleet,
with one active job per actor. Each job permits one HTTP attempt. Connections are authenticated
loopback JSON lines, bounded to sixteen sockets, 64 KiB per frame and bounded mailboxes. Responses carry
request IDs and can arrive out of order. Workers renew 15-second leases independently of HTTP work.
Lost leases, expired permits and previous-boot tokens cannot change memory. A pre-attempt release
can reassign a job without resetting admission time; provider failures fall back without retrying.

Each permit is appended and synced by an I/O thread before authorization. Pending and uncertain
attempts remain charged. The private ledger has a process lock and a pinned profile; reopening it
preserves the cumulative request count and recent reservations. The ordinary default is Limited at 30 model
calls per real minute. Unlimited removes RPM gating; explicit trial mode retains a finite reservation budget
(default 100). No restart refunds usage. Queue waiting expires after at most 20 real seconds; memory inputs also
retain their original admission deadlines (45 real seconds or 120 game seconds, whichever expires first).
Rejected memory work uses the original template fallback or retains its bounded ingress if fallback is unavailable.
Normal gameplay and memory retelling continue. Provider outcomes, token usage and latency are logged to the world
and worker logs; the ledger retains reservations. There are no automatic retries, model tools or token-rate governor.

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
health, equipment, bags, quests, memory counts and worker request counters. Interpreter policy controls require
a separate private control token; gameplay/simulation controls stay disabled on an ordinary realm. Samples older
than ten seconds are explicitly stale.
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
store by one save interval. "Talk to memory" submits a lower-priority, read-only interview through the shared
worker scheduler and model allowance. It combines selected committed memories with fresh personal state and current
intentions supplied by the world, and forms no memory or speech. There is no direct provider bypass. Configure
`Alles.Interpreter.ControlTokenFile` to enable admission. The bridge reads the tables through the `mysql` client with
the `CharacterDatabaseInfo` of its checkout's `worldserver.conf` and finds the worker model in `worker.json`
beside `Alles.Worker.TokenFile`; `--world-conf`, `--worker-config`, `--mysql` and `--no-memory` override
that. See `apps/observatory/docs/INTERFACE.md` for the `/api/memory` endpoints.

## Natural conversation

Delivered replies are journaled as `alles_conversation`, including the actual spoken text, route, proposed
local action and its result, worker job, dialogue thread and whether the speaker is a bot. The legacy `human`
context key identifies the other participant for existing journal consumers. Event value 1 requires the exact
comprehended text at that recipient's packet hook. This is server delivery evidence, not a client-display ack.

With bridge mode and `Alles.Conversation.Enable=1`, actually delivered, understood SAY/YELL, General, Trade,
whisper and party speech enters the conversation queue for eligible configured bots. General/Trade use server
DBC identities and exact channel-instance membership. Remote hearing captures sender identity without reading
unseen location, inventory or quests. Addon, system, raid and unsupported channel layouts are excluded.
Replies use the ordinary session chat handler, including normal language, mute, level, membership and script
checks. No HELP keyword or punctuation is required. A direct name at the start, optionally after a greeting,
routes to that actual listener; other phrasing is also evaluated by the worker. Otherwise one eligible listener
is elected, preferring relevant private evidence and rotating equal candidates.

Humans and bots can take part. Each responding bot gets its own identity, place, up to eight recent utterances
from the delivered exchange, and up to four matching memories ranked by salience. When objective knowledge is
available, up to six private geography entries and four source-attributed reports enrich activity/place retrieval.
Starting geography stays vague; a level band is not a verified quest or prey location. Reports preserve source,
age and uncertainty. Evidence is bounded to an 8 KiB serialized context. Private and public histories are kept
separate, and reversing the two participants' roles preserves only their actually heard dialogue on that route.
History expires after two idle minutes or logout; it is not a separately persisted transcript. Ordinary heard
speech still follows the durable memory path. Disable `Alles.Speech.Enable` to stop unsolicited memory recitals
while leaving conversation enabled. Routine deaths have very low priority and are not default conversation topics.

The response schema is `reply`, `text`, `action`. Text is limited to 255 UTF-8 bytes with no control characters
or client markup. Actions are `none`, `wave`, `follow`, `stop`, `assist`, and conditional `offer_help`;
arbitrary commands are impossible.
Available schema choices follow the supplied current capabilities, and the server checks live state again.
Remote and bot-to-bot conversations can speak or make an eligible recruitment offer. The following local actions
require a current human request and a compatible independent New RPG bot:

- Follow becomes an objective with the actual requester, exact statement and a two-minute deadline. The objective
  runtime holds independent routes and pulls while ordinary follow movement, combat and recovery continue.
  Strategies are preserved. Stop, requester logout, lost proximity or human control handoff cancel the request;
  the bot's own death pauses it. Reload reconciles the retained request without extending its deadline.
  Another player cannot take over an active follow through this path.
- Assist can engage only the exact visible nearby hostile NPC already fighting the requesting player when the
  job was created, if it is still eligible at delivery. It calls the existing playerbot attack action. It cannot
  select arbitrary targets, initiate PvP or attack a newly selected unrelated creature.
- Wave uses the normal emote and completes only when its packet reaches the requesting player. Assist completion
  requires observed own engagement against the exact threat. Immediate actions never replay after reload.
  Healing, trade, quests, party changes and teleportation are not local conversation actions.
  Failed action checks replace the proposed promise with a truthful failure line.

These requests use the same persisted objective book and capability validation as autonomous intentions.
Conversation enables this request service even when `Alles.Objectives.Enable=0`; that setting still controls
autonomous quest/exploration planning. `alles_objective` telemetry includes the request source, text and deadline.

Every response is fenced by both participants' current sessions and the current route. Local replies recheck
visibility, phase and range; remote replies recheck their actual channel/group membership. The world omits stale
or failed replies and records the outcome. Responses are staggered at least one second apart, with a short
respondent-specific delay before worker dispatch. There are at most 32 retained turns, three turns per speaker,
32 pending model jobs and one pending job per bot. Ordinary turns expire after 30 seconds; recruitment turns
have two minutes to process the separately elected responders.

Each dialogue lasts at most two minutes and permits four reply attempts between two participants. Other bots
may hear an answer, but it offers a follow-up turn only to its intended peer. Each bot has four attempts per
minute and each audience six, shared across threads. Failed jobs consume these limits. Repeated normalized
topics are suppressed for the thread lifetime; recently delivered identical or nearly identical answers are
suppressed for one minute. These bounds complement the worker's instruction to stay silent when an answer
adds nothing useful. There are at most 128 threads, histories and recent-answer entries.
Recruitment allows one attempt each from up to four distinct eligible responders, processed one at a time.
Their identically worded offers remain distinct evidence. The same per-bot, audience and total attempt limits apply.

With `Alles.Objectives.Enable=1`, an information-blocked deferred objective can ask about missing quest
information or work elsewhere. It must have an ordinary available audience and an idle opportunity outside
combat or active quest execution. The question includes only its own quest/place and level. Party, General,
Trade and local audiences use actual membership or current local hearing; the session handler still decides
whether speech is permitted. An objective gets at most two attempts, at least ten minutes apart, with a two-minute
reply window. The attempt enters the owner snapshot before emission and uses the normal asynchronous save path.
A relogged snapshot retains the attempt count. A question or received line never establishes quest progress.

`alles_question` records whether any other player received the exact comprehended text. Objective telemetry's
`information` distinguishes pending, undelivered, awaiting reply, unanswered and a validated lead. Up to eight
candidate replies per question are retained for interpretation, with bounded storage and expiry; they are not
automatically promoted into useful facts. A version-1 planning job classifies each candidate using only the
question, delivered line, installed capabilities and supplied references. Place references resolve whole names
actually present in the line; ambiguous unknown names are omitted. The worker receives no global place list,
coordinates or pretrained quest guide. Its response can ignore the line, retain a report, queue investigation
of a reported place, or reconsider an accepted quest after concrete advice. The world checks the owner generation,
objective revision, question attempt/deadline and supplied references again before applying anything.

Reports retain the exact statement, speaker, receipt time and uncertainty. Warnings can be retained without
reopening an objective. A usable place lead queues investigation while leaving the exhausted parent deferred;
it preserves current execution and waits behind outstanding turn-ins. Arrival alone does not complete an
investigation: actual new work in the intended area is required. Personal visits and useful work assess reports
separately. Repeated reports do not refresh their age, raise their confidence or repeatedly reopen a retry.
`alles_advice` records application/rejection and report/intention references; `alles_objective` records subsequent
observed execution. The classifier's conversational quality and gameplay follow-through still require live testing.

Ordinary objective choices also use the planning contract (`purpose=decision`). The runtime offers bounded
options from the owner's accepted quests, private geography and current obstructions, with associated reports.
It reacts to quest progress, state changes, deaths, learned/assessed reports, retry availability, level and equipped
item/permanent-enchantment changes. Stable elapsed samples do not trigger calls. Each owner waits for a short
settling period and at least 30 seconds between admissions; advice and ordinary decisions share one pending slot
per owner, in addition to the global worker bounds.

A decision can prefer an available quest or place, choose an eligible information question, defer measured
unproductive execution, or keep the current plan. Preferred work is saved with `plannedMs`, without advancing
counters or interrupting current execution. When free, a bot handles outstanding turn-ins first and then its
preferred work. A recent viable queued preference holds for two minutes to limit destination switching.
The world checks both the job's anchor objective and the chosen option's revision and preconditions; changing
quest state, handoff, expiration and unsupported option/reference combinations reject the choice. A deferral
retains the quest/intention and its checkpoint. The normal question sender still records delivery separately.
`alles_plan` distinguishes selected preferences, released execution, requested questions and rejected results.
If the worker is unavailable or returns no useful choice, existing deterministic quest/exploration continues.

Own readiness gates quest selection and retries. Quests more than three levels above the bot's level defer with
a strength obstruction; critically damaged equipped items (at most 20% durability) defer combat work for repairs.
When all other turn-in credit exists but required money is missing, the obstruction is supplies. A failed quest
uses prerequisites, unsupported quest types use executor failure, and eligible group quests use companions.
Strength, supplies and prerequisite deferrals require an observed resolution before retrying; time or an unrelated
equipment change cannot reopen them. A remaining own condition can update the diagnosis without resetting attempts.
When an accepted quest has all other turn-in credit but lacks its required payment, the fallback keeps outstanding
turn-ins first, then favors feasible accepted quests with positive advertised money rewards before unrelated leads.
It retains valid current work and uses ordinary quest execution, loot and reward handlers to earn the money.
The choice records why it is pursuing paying work. Actual balance and accepted quest rewards/costs also enter the
bounded model context; only supplied own quest references are exposed. Expected rewards never increase the balance
or satisfy readiness. Reconsideration follows real balance changes, and the unpaid quest can resume only after
the existing readiness sampler observes enough own funds. Telemetry identifies `earning_quest_money` while this
need persists. This does not invent available jobs or guarantee that a projected gross reward covers incidental costs.
Equipment repair now has a saved preparation phase on the parent quest. When free of other work or cooperative
commitments, an autonomous solo bot can approach an already-visible repair NPC within 60 yards through ordinary
following movement. It repairs only critically damaged equipped items using the normal session handler and its own
money. Outside instances, it remembers its own position when a friendly visible repairer is actually interactable.
If no repairer is visible later, the same preparation can return along a generated walking path to the nearest
personally observed site within 600 yards on its current map and phase. Starting geography and hearsay never grant
these locations. A remembered site is only a place to look again: arrival still requires a newly visible,
interactable repairer, and an absent service, failed route or expired attempt defers without spending remotely.
The preparation deadline is two minutes, with one transaction batch per attempt and at most three attempts
under unchanged funds, separated by ten-minute deferrals. An observed increase above the funds at the last attempt
allows reconsideration; reload, spending and the first observation of a legacy balance do not reset this limit.
Total attempts and expenses remain recorded. The saved charge prevents transaction replay after reload. Actual equipped
durability determines whether the need is resolved; the transaction return value cannot complete preparation or
advance quest credit. Telemetry records preparation state, reason, attempts and actual money spent. Human control,
cooperative control and parent cancellation stop the solo preparation. No NPC pointer or movement handle persists.
Own repair locations persist with observation time; transient routes do not. Telemetry distinguishes returning to
a known repair site from approaching a currently visible repairer and completing a paid repair.
The worker may select the supplied `repair_equipment` capability; the same bounded deterministic fallback works
without the worker. Legacy repair and cached independent movement respect the preparation hold.

Solo bots can also buy missing required quest items from a merchant they can already interact with. Offers come
from the normal delivered inventory menu; merchant stock, prices and handles are temporary. The saved
`buy_quest_supplies` intention names the actual own quest requirement. Before a charged attempt, the runtime opens
the menu again and checks current stock, bundle quantities and own money, reserving money for otherwise ready
quest turn-ins. Purchases use the normal session handler and avoid offers requiring other currencies. Actual own
inventory resolves preparation and actual quest state supplies credit. Repair and purchase share bounded deadlines,
fund-change retries and transaction replay protection. If funds are short, the same charged attempt can sell at most
four expendable poor-quality miscellaneous bag stacks through the normal vendor handler. Quest requirements and
source items, usable items, equipment, bags, wrapped/refundable/trade items and bank contents are protected. Supply
purchases sell only enough units to cover the current deficit; after a failed paid repair, a bounded junk sale can
fund another normal repair pass. Actual item removal and currency increase establish income; projected proceeds
never authorize a purchase. Saved income and expenses remain separate, including when spending exceeds the initial
cash balance. Combat and incapacitation pause resource actions. The supply keeper, including its zero-money funding
variant, has compiled but has not run live. Earning funds without expendable inventory, distant service discovery
and coordinated party preparation remain pending.

During an owned creature-kill step, visible already-tagged targets or matching corpses can pause the active-work
timer for at most one minute. A newly visible eligible target resumes the same attempt; expiration defers the quest
with a competition obstruction. Empty or truncated scans do not assert a spawn timer or infer hidden competitors.
The wait deadline survives reload. Navigation failures keep their specific cause instead of a generic no-progress
timeout. Live readiness/competition acceptance remains pending; the new readiness keeper has only compiled.

Cooperative intentions now have a persistent owner/quest-bound agreement state and normal party-operation
adapters. Agreement alone cannot enable execution: actual ordinary-party membership, compatible accepted/shared
quest state, arrival and readiness must be observed. Each agreed participant's own reward is required for collective
completion. On reload, saved readiness/completion is reconciled against current group and quest state. The control
classifier permits an independent bot master in the same ordinary party and yields to human/selfbot/external
control. Objective-managed bot-to-bot invitations use the agreement adapter. A real human's incoming invitation
still follows normal security and acceptance, allowing that human to become the master and suspend the objective.
The corresponding handoff/resumption keeper has compiled but has not run live.

Accepted ordinary elite/group quests defer with a companion obstruction instead of retrying solo. An eligible
`seek_companions` choice or fallback emits a bounded normal question naming that quest and a privately known
meeting area. The question's semantic attempt and recruitment intention precede packet emission. Only an actual
delivered reply can enter `purpose=recruitment`: the available choices are `none` or `invite_companion` for that
speaker and quest. The worker must distinguish clear willingness from refusals, ambiguity, unmet conditions and
third-party suggestions. A valid offer is retained verbatim; `alles_advice.agreementPublished` records publication
of that private intention. Once enough companions agree, normal whispers deliver the full roster to each member
before any invitations. Bots retain additional peers as the leader's attributed report. Every bot must adopt the
delivered roster; humans must receive it and decide whether to accept the subsequent ordinary invitation.
Roster whispers share the existing chat budgets. `alles_roster` separates delivery from adoption, and
`alles_cooperation.invitationObserved` distinguishes an observed invitation from membership or quest progress.

An actual delivered recruitment question can offer a responder the conditional `offer_help` conversation action.
Its own quest eligibility, readiness and commitments determine availability; it may decline. A valid offer retains
the received quest/meeting declaration as uncertain information and records the responder's own agreement before
speaking. Failed delivery conveys no consent to the recruiter. Matching normal invitations can then be accepted,
and the agreed quest can be shared and accepted through normal handlers. Acceptance requires a transient receipt
for that exact sender/quest, not merely the core's sender-only pending field. An unshared quest may wait within
the agreement deadline; actual acceptance and later abandonment remain distinct observations.
An offer whose quest was never accepted can be retried after ten minutes, at most twice for that retained
objective; relog does not refund attempts. Actual quest abandonment remains cancelled. Helpers who have already
earned their reward can offer support while retaining that completed outcome and observing peers' progress
separately. Pending turn-ins and conflicting commitments still take precedence.

Rendezvous uses ordinary pathfinding toward the known meeting area or an actual party member. Party positions
remain executor-only. A lack of measured advancement defers the agreement; managed movement cannot use legacy
stuck teleport recovery. A working party pauses for recovery/catching up without forgetting its rendezvous.
The work lead stays with an incomplete participant, then shifts to another who needs credit, and finally to
outstanding turn-ins. Followers hold independent pulls/routes while ordinary combat and loot remain available;
they can assist a visible member's engaged creature. Normal disbanding does not erase observed completion.

Existing unagreed parties cannot acquire quest execution through these gates. Source/unit checks do not
establish live party formation or completion; those acceptance gates remain outstanding.

Memory, objective planning, conversation and interview jobs share fair per-actor scheduling with bounded waiting.
Workers negotiate protocol 2 and `planningVersion: 1`; planning uses `submit_planning` and a closed versioned response.
The worker profile fingerprint includes this contract, so deployment requires the matching world profile and
its corresponding ledger; existing runtime configuration and ledgers are not rewritten automatically.
Continuous local play defaults to `Alles.Interpreter.BudgetMode="limited"` and
`Alles.Interpreter.RequestsPerMinute=30` (range 1–100000; `rolling` remains an accepted legacy spelling).
Reservations count against a trailing one-minute window and replenish as they age out; failures are not refunded.
Timestamps and cumulative request count survive a worker/world restart. The private ledger is compacted into a
synced checkpoint before 1 MiB, keeping the cumulative count and recent time buckets under a stable lock file.
Old trial ledgers remain valid in trial mode and do not reset themselves. A changed prompt/profile needs a new
recorded ledger. The dashboard distinguishes Limited, Unlimited and lifetime trial counts.

`cmd/alles-fixture-provider` supplies deterministic responses for exclusive acceptance trials through the normal
interpreter's HTTP provider adapter. It never connects to a model, bridge, database or world itself. It serves
`/models` and `/chat/completions` on IPv4 loopback only. Run these commands from `apps/alles`:

```sh
go run ./cmd/alles-fixture-provider --fixture testdata/deterministic-provider.json --describe
go run ./cmd/alles-fixture-provider --fixture /absolute/path/to/scenario.json \
  --journal /absolute/path/to/new-attempts.ndjson --listen 127.0.0.1:11435
```

`--describe` validates the file and prints its `base_url` and `model` without opening a listener or journal.
The model identity includes the full rule-file SHA-256, so changing a scenario requires recomputing the ordinary
worker's profile fingerprint. Use a separate worker configuration and fresh bridge trial ledger for this phase;
preserve the real-model configuration and its 100-request trial allowance. Starting this phase still requires
the same deployment/gameplay authorization as other live acceptance work.

Rules match the response schema and exact object-key paths in the delivered user context. Each request must match
exactly one rule; absent, overlapping or exhausted rules return an error. A rule returns a fixed JSON object or
copies an existing object such as the supplied memory draft. It cannot interpolate references, query hidden data
or perform an action. The ordinary worker and worldserver retain their capability, evidence and revision checks.
Per-rule limits and a global cap of at most 100 POST attempts include rejected requests. Every admitted attempt's
rule, result and input hash are flushed to a new exclusive journal before replying; journal failure stops admission.
The journal does not store raw player speech. Restart with a new journal and retain the old one as trial evidence.

The example retains memory drafts, greets one exact named fixture input once and keeps existing planning decisions.
It is a starting example, not a passing advice/cooperation scenario. Prepare case-specific rules from actual
fixture identities, issued references and expected delivered messages under `e2e/local/`; use ordinary client
traffic to trigger them. Prove gameplay with the keeper oracles, then repeat the scenario with the real model to
assess judgment. Provider tests, canned speech and a matched journal rule alone do not prove gameplay success.

Offline conversation evaluation uses the same schema, client and separate durable fixture budget:

```sh
alles-interpreter --config FILE --conversation-fixture CHAT.json --fixture-ledger EVALUATION.ndjson --max-requests 20
```

The fixture is `{"jobToken":"fixture","permitId":"fixture","remainingMs":25000,"context":{...}}`;
context has `bot`, `player`, `message`, `selfContext`, `place`, `audience`, `history`, `relevantMemories`,
`inCombat`, `followingPlayer`, `canFollow`, `nearbyThreat`, and `capabilities`.
