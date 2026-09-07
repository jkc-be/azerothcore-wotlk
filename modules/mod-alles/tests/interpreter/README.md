# In-process pilot coordinator

`Alles::Interpreter::PilotCoordinator` takes a world-thread `ActorStore&`. Track configured owners with `Track`,
call `Update(gameTimeMs, realTimeMs)` after lifecycle/load processing, and remove only drained owners with `Forget`.
Keep configured owners tracked through ordinary store eviction and handoff so their rate bucket survives.
`Forget` refuses Loading owners because they may hold observations outside the ready snapshot.
The default fake queues a value result after dispatch; acceptance occurs on a later update through the same
`Submit` queue as injected proposals. `Inspect` returns an owned job copy for diagnostics and fixtures.
No gameplay object pointer, database credential, socket, provider call or provider billing ledger enters this API.

The pilot uses a five-game-second debounce, FIFO ready owners, at most eight ordered perceptions and twelve own
memories per job, and a conservative 24 KiB bound for its context values and metadata. Stable fake overhead is
included; W4b/W5 must enforce the limit on their complete actual serialized prompt, including their own stable
prefix. The context prioritizes pending inputs and then highest-salience own memories. Inputs too large for a
bounded job receive an explicit local fallback without being dropped.

At most four proposals are allowed. Adjacent inputs can share one memory only when their authoritative kind,
exact typed subject/source, perceived place, language/comprehension and audible attribution/depth match. The fake
and fallback join every supported claim in order, at most 512 Unicode characters per memory. A long or
incompatible input closes that particular batch sooner when it would require a fifth output; the general
batch cap remains eight. The validator requires complete, nonduplicate support covering the exact ordered input
prefix and rejects groups that mix incompatible sources. Model wording is still untrusted; support-token
coverage cannot prove that arbitrary prose faithfully summarizes every input.

Jobs preserve their oldest accepted input's 45-real-second and 120-game-second deadlines, including queue wait
and debounce. Standard admission uses a four-per-minute owner bucket with burst one: one grant every fifteen
real seconds, charged at first dispatch. `met` is ordered Reflex and cannot bypass an earlier logical job.
Owner and global queues are bounded. Default concurrency is one.

Core `CryptoRandom` generates independent boot epochs, job tokens and fake permits. Actor generation, job token,
worker identity, pinned fake profile, lease generation and fake permit are all independently checked at
processing time. Fake permits use the `fake:` namespace and leave `httpAttemptCount=0`. Initial leases last
fifteen real seconds, with five-second fake heartbeats when enabled; permit lifetime is twenty-five seconds.
Renewals cannot extend either absolute admission deadline or permit lifetime. Fake behavior is copied at grant,
so changing fixture settings cannot rewrite an active job.

For an input admitted at t=0 and dispatched at t=5, `Withhold` with heartbeats normally falls back at t=30:
its fake permit expires 25 seconds after dispatch, before the admission deadline at t=45. Without heartbeats,
the initial lease expires at t=20. A queued or pre-debounce input can instead exercise the full t=45 admission
bound. These are distinct expiry fixtures. Heartbeats never buy additional permit time, and a valid proposal
queued before t=30 but processed at t=30 is rejected. The fake's configured response delay must leave time for
both enqueuing and a later acceptance pass before all applicable boundaries. A late heartbeat cannot revive an
already expired lease. `expiredJobs` counts all lease, permit and admission expirations collectively.

A stale or duplicate proposal does not apply or consume anything. A current malformed proposal causes village
fallback through the original prefix. Lease/permit/admission expiry is processed before queued results, so an
early receipt cannot authorize a late application. Acceptance rechecks every supplied memory's content revision;
rehearsal or decay that changes only salience does not import an old numeric value over the current memory.
An observed prefix/generation change or lost/semantically changed supplied memory immediately fences the invalid
job and releases its worker slot (`invalidatedJobs`). Retained inputs can obtain a new immutable job, with the
same original admission times and without refunding the owner's already charged rate token. An old result still
cannot apply to that replacement. Already-expired jobs with a valid prefix template their original whole batch
rather than admitting a replacement.

`Stop` fences and clears jobs/results while leaving perceptions in ActorStore. A village shutdown can call
`DrainFallback` in bounded passes, then use the existing snapshot shutdown path. Strict labs must retain their
pending inputs and must not call this village fallback drain. W9 pause/hold semantics are not implemented here.

The coordinator accepts `create`, `reinforce` and `supersede` through one `ActorStore::ApplyMutations`
transaction. It first grounds the complete ordered support partition, kind, typed entity/source/place references
and local confidence caps. Targets resolve only from that immutable job's supplied memories. The domain validates
the exact current input prefix, generation, every supplied memory revision, target ownership/uniqueness, numeric
bounds and ID/revision capacity, then stages all mutations and publishes one snapshot revision. `Apply` remains
a create-only wrapper for Reflex/fallback callers. The internal domain mutation values are already grounded by
the coordinator; they are not a wire decoder. W4b's decoder must additionally reject unknown serialized fields.

Reinforcement requires the same retained claim, kind and subject. It decays the current target and adds a fixed
local salience increment of 0.1, capped at one, preserving its current confidence, wording and provenance. It
ignores proposed absolute salience/confidence values after range validation; a new speaker cannot overwrite
remembered provenance or transfer hidden numeric confidence. Broader semantic matching is outside this pilot.
Supersession keeps the supplied target ID, advances its content revision, assigns the new supported wording/kind/
provenance and capped confidence, and preserves the target's current decayed salience. Neither operation can
resurrect a forgotten target or undo provenance erosion observed during staging. Supersession records a new
formation timestamp; reinforcement records rehearsal time. Allocation or validation failure before publication
leaves the store unchanged. Existing older save snapshots remain immutable and cannot restore replaced contents.

Unknown or duplicate mutation targets fence the job without changing memories or consuming inputs. The original
admission times and charged owner bucket govern a replacement job or eventual deadline fallback. Malformed
kind/text/scores/reference combinations still use village fallback over the original prefix; they never apply
part of the rejected proposal. `fakeMemories` counts successful creates, while `fakeReinforcements` and
`fakeSupersessions` count those operations separately. The default fake generates creates; injected value fixtures
exercise all three through the same acceptance route. This is bounded interpretation, not W8 consolidation.

Authored gtests cover eight-input aggregation, heterogeneous/long batch boundaries, immutable delayed context,
epoch/worker/generation/lease/permit fences, complete support, invalid scores, lease and admission expiry,
ordered Reflex, equal typed IDs, FIFO dispatch, owner token rate, memory revision fencing, rehearsal, fake modes,
loading-owner retention, invalidation/replacement scheduling, shutdown, mixed atomic mutations, duplicate/foreign/
stale/forgotten targets, counter exhaustion, and preservation of current local numbers and provenance. All 25
coordinator tests and nine atomic mutation tests compiled and passed on 2026-09-07. W4b adds authenticated sockets,
durable provider permits, worker release/reassignment and status receipts. W5 adds model/provider validation.
First light still requires a
watched death-to-rumor scene plus timeout and save/relog proof; these passing unit tests do not
establish that gameplay has passed.
