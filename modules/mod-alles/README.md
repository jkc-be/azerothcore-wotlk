# mod-alles

Implementation of the alles village MVP is in progress. The ordinary-realm pilot connects bounded memory storage,
local player perception, an optional Ollama interpreter, ordinary bot speech, self recall and read-only live telemetry.
The C++ in-process fake remains available for deterministic testing. Native world builds and all 108 Alles unit tests
passed on 2026-09-07, including the external permit, wire and durable-ledger checks.
Full first-light and broader fleet/lab validation remain pending.
`Alles.Enable=0` remains the default. Configured player owners run on an ordinary realm; NPC owners and accelerated
Observatory mode are rejected. See [the external worker and telemetry guide](../../apps/alles/README.md).

The module requires static linkage for both mod-alles and mod-playerbots. Its migration lives in
`data/sql/characters/alles_0001_owner_memory.sql`. A binary compiled with the module needs that schema even when
gameplay is disabled at runtime. The normal updater discovers enabled module SQL before preparing statements;
deployments with updates disabled must install the migration before starting the binary.

The guarded CharacterDatabase extension preserves existing statement indices and exports a fingerprint of every
statement name and index. Module registration and startup compare the module's own compiled layout to the database
library. See [the integration gates](tests/integration/README.md) for the build matrix and deliberate mismatch test.

`src/domain/` contains world-thread value logic:

- Owners use `(kind, uint64 ID)`, with player kind 0 and persistent creature spawn kind 1. Equal numeric IDs
  remain separate. Transient subjects can have a name without a persistent ID.
- Speech is language-gated before retention. Unintelligible plaintext is discarded. Heard statements never gain
  another character's numeric confidence or hidden transmission history.
- Fallback recognizes the exact audible English forms `I saw ...`, `<name> told me ...`, and `I heard ...`.
  The first two suggest reported depths 1 and 2 respectively; this is audible attribution, not verified history.
  Other wording leaves depth unknown. The immediate speaker and explicitly named attribution remain distinct.
- The initial local hearsay cap is 0.6; deterministic heard claims start at `min(0.5, cap)`. There is no human/bot
  flag in formation. Rehearsal increases salience without treating repetition as new corroboration.
- Salience decays exponentially from its last checkpoint, initially with a 24-game-hour half-life. A second pass
  at the same time cannot decay it again. Below salience 0.2, heard provenance and opaque quoted content are erased
  from the stored memory itself. Below 0.01, the store forgets the memory. These initial policy values await tuning.
- Owner generations fence stale loads and results. Runtime attachment tokens are separate, so an old logout cannot
  close a newer incarnation. Loading buffers are bounded ingress; merge overflow is counted and cannot displace
  already retained inputs. No empty store can flush before a successful load.
- Complete owner snapshots consume perceptions and apply creations, reinforcements and replacements together.
  Accepted input order, typed targets and supplied memory revisions are checked before a staged snapshot publishes.
  Loaded rows are validated before installation. Reinforcement changes current local salience while preserving
  confidence/provenance; replacement retains its target ID and advances its content revision.
- Snapshot scheduling starts after 30 real seconds dirty, or 64 material changes with a five-second minimum.
  Explicit flush/close requests bypass ordinary scheduling and coalesce with an already captured revision.
  Drained offline owners stop ticking decay until their final snapshot commits and can be evicted; elapsed
  decay is applied on load/reattachment. Offline pending interpretation continues to decay its context.
  A bounded round-robin pass shares four transaction slots, with one transaction per owner. Failed saves retain
  dirty state and retry after five real seconds; completion only acknowledges the captured revision.

Defaults bound the store at 256 managed owners, 256 memories and 128 pending perceptions per owner. Speech content
receipts use a fixed 30-game-second window; repeated lines do not extend it. Emission IDs have a separate bounded
receipt cache. Overflow never evicts input already admitted to a job.

`src/perception/` separates ephemeral packet decoding from live capture. The adapter snapshots the receiver's
language knowledge, comprehension aura, perceived names/place and bounded self context before enqueueing a gated
value. Source lookup uses only the receiver's visible object map. Death witnessing checks map, phase, visibility
and range independently for victim and killer. Meetings use a bounded visibility scan and cooldown. Eight text
emotes currently have deterministic renderings; animation packets cannot create a second emote.

The runtime captures player chat only on the world thread. The current core schedules map workers and waits for
them in `MapMgr::Update`; `WorldScript::OnUpdate` runs after their join. Other-thread packet callbacks are omitted
before reading the receiver, with an `unsafe_packets` counter. This is incomplete hearing coverage: map-worker
chat/emotes need a proven delivery context. NPC chat is also omitted until W7 supplies emission-scope metadata,
because zone/map announcements can use local-looking packet types. Pure decoder support does not authorize them.

`src/runtime/` keeps lifecycle and perception values in one bounded, mutex-protected ingress queue. Its reserved
lifecycle capacity cannot be consumed by sensory traffic. Per-session attachment tokens prevent a delayed old
logout from closing a new body. A lifecycle overflow suspends interpretation and speech and reports an error.
The world pass processes bounded ingress and DB completions, resolves live bodies for the current call, and owns
all store/coordinator mutation. Configured offline owners are loaded at startup to discover persisted inputs.
Cache eviction preserves the configured owner's dispatch rate limit.

`src/interpreter/` implements the pilot coordinator and C++ fake worker. Jobs carry at most eight ordered inputs,
twelve own memories and four proposed mutations. Compatible adjacent inputs may share one output. Tokens, leases,
fake permits, supplied revisions, ordered support and admission deadlines are rechecked before atomic application.
See [the coordinator contract and limits](tests/interpreter/README.md) for reinforcement/replacement policy.

Eligible autonomous playerbots retell an own witnessed death or heard claim through ordinary `Player::Say`.
Human/selfbot/external-control bodies do not speak autonomously. Selection can favor a recent admitted audible
question naming a memory's subject. At most two actors attempt speech per update, with per-owner cooldown and
repeat suppression. Lines exceeding 255 bytes are omitted. Audience includes every nearby online player who can
see and hear the speaker, independently of `Alles.Owners`. Actual delivery to an unmanaged human also acknowledges
rehearsal and repeat suppression, without creating a memory owner for that human. Configured recipients capture
only the emitted line and independently perceived speaker.

Local SAY/YELL/emote decoding accepts both ordinary and GM chat packets. Core selects the GM packet layout from
RBAC permission 372 even when a privileged account plays normally with `.gm chat off`. Both layouts retain the
same visibility, language and range gates. `/who` is not a global hearing channel; distant online players remain
outside local speech range. This ambient path retells memories. The separate opt-in [natural conversation path](../../apps/alles/README.md#natural-conversation)
interprets ordinary nearby player speech and can propose a bounded set of gameplay actions.

After an authorized build and setup, `Alles.Owners` accepts 1-64 unique entries such as `player:42,player:43`.
Use the six actors prescribed by the first-light plan. `.alles recall [1-20]` reads only the caller's current
character memory. GM/console `.alles status player ID` reports lifecycle and committed revision;
`.alles flush player ID` requests persistence and reports a revision to wait for. A pending request or core `.save`
alone is not a durability acknowledgment. General `.alles status` reports capture omissions.

`src/storage/` supplies prepared asynchronous owner loads and transactional snapshot writes. Successful empty
queries contain sentinel rows, so SQL failures cannot masquerade as missing owners. Reads are bounded and compare
actor metadata before and after loading children. Shutdown retains older futures and checks the committed revision
after a final synchronous write. The core's synchronous connection acquisition and DB calls cannot currently enforce
a hard deadline once entered; see [storage sequencing and limits](tests/storage/README.md).

Tests live outside `src/` and join AzerothCore's `unit_tests` through `ACORE_MODULE_TEST_SOURCES`. Configuration and
compilation require explicit authorization. All 97 alles tests and the deliberate layout-mismatch CTest passed
on 2026-09-07. Local native probes also passed headless hearing, distant non-reception, explicit flush and
self recall after confirmed cache eviction and a fresh DB load. These are not the committed Go e2e suite.
First light still requires one save/relog, expiry fallback, and an
observed Northshire death-to-rumor scene on the native stack. Runtime visibility, language, handoff and shutdown
failure-injection proofs are still outstanding. The external one-slot pilot covers a subset of W4b/W5 and ordinary read-only telemetry.
The broader W4b-W10 requirements remain in scope; this source slice does not satisfy them.
An opt-in [live persistence fixture](../../e2e/suites/alles/README.md) now has source coverage for player SAY,
self recall, committed revision acknowledgment, distant non-reception and reload after actual cache eviction.
The persistence fixture has been compiled but not executed. The separate online-human audience fixture passed
on 2026-09-07, including privileged local SAY/YELL and autonomous speech to an unmanaged client. Neither replaces
the six-actor watched first-light scene.

## Learning from consequences and keeping useful memories

Personal injury retained when an activity is abandoned, and death during an intention, teach that actor's
activity/context estimates. Death also records failure of the observed route. Recovery restores current health
without erasing the lesson. Contextual probability and consequence estimates lose half their influence after six
game hours without new evidence; later successful attempts can revise them. Successful capabilities still transfer
between contexts, while a harmful destination does not teach that every destination is dangerous. Planning payload
version 14 retains evidence timestamps and reads earlier versions without discarding their learned outcomes.

Direct sightings occupy at most half the memory capacity. Repeated sightings of one persistent identity refresh
its encounter count, last-seen time and place; creatures sharing a name remain distinct. These familiarity records
cannot evict retained personal episodes. Sightings say “I saw”, and personal deaths retain high initial salience.
The dashboard separates episodes and familiarity through the Show filter. Apply the character migration
`data/sql/updates/pending_db_characters/rev_1789063646525460671.sql` when upgrading the memory schema.

A delivered greeting acknowledgement closes a greeting-only thread. A new question or a reply containing useful
additional content can continue conversation. Visiting a companion completes only after a nearby reply is heard;
merely delivering the greeting does not award companionship. Help and quest progress retain their ordinary
execution, cooperation and observed-progress guards.

The planner skips requests that merely endorse a valid ongoing activity. Meaningful need bands, new outcome
knowledge and objective changes can trigger reconsideration. A finished or expired request is not retried solely
because the same observation is sampled again. Rejections distinguish expiration, missing/changed objectives and
ownership changes; option, evidence and generation checks still apply.

### Progress and comfort

The WoW adapter values mastery (accumulated XP across levels), carried wealth and equipped item levels as
individual growth ambitions. Rest, safety and social needs remain bounded needs. Accepted quest forecasts use
the owner's known XP/money rewards and usable equipment upgrades; unknown work retains an uncertain prior.
Actual player state alone updates these measured ambitions. Starter items, shields and ranged weapons count;
shirts and tabards do not. Item level is a progress proxy, not a simulation of combat effectiveness.

Living health changes include both wounds and recovery, so a healed fight does not teach permanent health loss.
Observed death records a separate failed outcome; resurrection is excluded from learned healing. Route travel,
perceived danger and prior outcomes still affect the choice, and staying remains valid when useful work does not
justify its costs. These game measurements live in the adapter; the satisfaction evaluator accepts arbitrary
need and growth dimensions for other scenarios.
