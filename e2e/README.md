# AzerothCore live-stack e2e

Protocol-level regression tests for this repository. They import
[`github.com/azerothcore/AzerothGhost/e2e/e2eharness`](https://github.com/azerothcore/AzerothGhost)
and run against a **live** authserver + worldserver + MySQL.

Offline `go test ./...` (without `-tags=e2e`) skips these packages.

Authoring rules for new tests live in the harness:

| Doc | Audience |
|-----|----------|
| [LLM_GUIDE.md](https://github.com/azerothcore/AzerothGhost/blob/v1.0.8/e2e/LLM_GUIDE.md) | Compact MUST/NEVER + APIs (LLMs and humans) |
| [EXAMPLES.md](https://github.com/azerothcore/AzerothGhost/blob/v1.0.8/e2e/EXAMPLES.md) | Full recipes and skeletons |
| `.agents/docs/e2e-policy.md` | When to add e2e vs unit tests (agent/review policy) |

---

## Prerequisites

1. Running AzerothCore **3.3.5a** authserver + worldserver.
2. MySQL with `acore_auth`, `acore_characters`, and `acore_world` (world DB is required for spawn cleanup and many fixtures).
3. Go **1.26+** and network reachability to auth (default `127.0.0.1:3724`).

Accounts are created by the harness (GM level 3, password `test`). Do not reuse real player accounts.

### Local harness (optional)

For co-development against a local AzerothGhost checkout:

```bash
cd e2e
cp go.work.example go.work   # gitignored; edit the replace path
# replace github.com/azerothcore/AzerothGhost => /path/to/AzerothGhost
```

`e2e/go.mod` pins `github.com/azerothcore/AzerothGhost v1.0.8` (see `go.sum`).
`go test` / `go mod download` fetch that module.

---

## Environment

Copy and adjust [`e2e/.env.example`](./.env.example). Stock AC and CI use `acore:acore`.

| Variable | Default (harness) | Meaning |
|----------|-------------------|---------|
| `E2E_AUTH_ADDR` | `127.0.0.1:3724` | Auth / realm-list address clients use |
| `E2E_AUTH_DSN` | `acore:acore@tcp(127.0.0.1:3306)/acore_auth` | Auth DB |
| `E2E_CHAR_DSN` | `acore:acore@tcp(127.0.0.1:3306)/acore_characters` | Characters DB |
| `E2E_WORLD_DSN` | `acore:acore@tcp(127.0.0.1:3306)/acore_world` | World DB (spawns, tele names, cleanup) |

Optional filters (`internal/meta`):

| Variable | Effect |
|----------|--------|
| `E2E_TAGS` | Comma list; test must include **all** listed tags (AND) |
| `E2E_SKIP_TAGS` | Skip if test has **any** listed tag |
| `E2E_ISSUE` | Run only tests with matching `TestMeta.Issue` |
| `E2E_RUNTIME` | Run only `short` / `med` / `long` |
| `E2E_ALLOW_SOFT_PASS` | **`1` only for local debug.** SoftPass is fail-closed by default |

Export vars in your shell (or `set -a; source .env; set +a`) before `go test`.

---

## How to run

**Recommended full suite** (pad-safe defaults: serial tests **and** packages):

```bash
cd e2e
# export E2E_* from .env.example first
go test -tags=e2e ./... -count=1 -v -timeout 120m -parallel 1 -p 1
# or:
make e2e-full
```

`-parallel 1` keeps in-package tests serial (`meta.Begin` is serial by default). `-p 1` runs one package at a time so IsolationPads never hash-share. Raising either without a unique preferred pad per concurrent package causes thrash.

### Make targets

```bash
make e2e-smoke                          # smoke/ + suites tagged smoke
make e2e-category C=quests              # one category tree
make e2e-sub C=spells/aura              # one leaf suite
make e2e-issue N=26549                  # TestAC_26549_* anywhere
make e2e-tags TAGS_FILTER=smoke,short
make e2e-full                           # ./... long timeout
make e2e-list                           # list tests
```

Override parallel / timeout:

```bash
make e2e-full PARALLEL=1 P=1 TIMEOUT=120m
# P = go test -p (packages). Keep 1 unless every concurrent suite has a unique preferred pad.
```

### Single test / package

```bash
go test -tags=e2e ./suites/combat/pets -count=1 -v -timeout 30m -parallel 1
go test -tags=e2e ./suites/... -run TestPets_SummonWaitDismiss -count=1 -v -timeout 15m
```

---

## Layout

| Path | Role |
|------|------|
| `smoke/` | Fast login / tele / relog guards |
| `suites/<category>/…` | Hierarchical scenarios (directory = category) |
| `suites/<category>/` | Suites by domain; issue guards named `TestAC_<n>_…` live beside related tests |
| `local/` | **Scratch/debug only** — gitignored (except `local/README.md`); never commit throwaways |
| `internal/meta` | `TestMeta` + env tag filters + `Begin` (Parallel policy) |
| `internal/fixtures` | Re-exports pads / `PackagePad` for suites that prefer fixtures |

Every live test uses `//go:build e2e` and should call `meta.Begin(t, meta.TestMeta{…})` before expensive setup.

### Scratch / agent debug (`local/`)

When validating a fix on a **live** stack (player-visible combat, protocol, quests, multi-bot), prefer writing a small e2e under **`e2e/local/`** instead of ad-hoc GM spam or long manual checklists. That tree is **not committed**.

```bash
# create e.g. local/repro/repro_e2e_test.go  (//go:build e2e)
make e2e-local
# or:
go test -tags=e2e ./local/... -count=1 -v -timeout 30m -parallel 1
```

If the scenario should stay as a regression, **move** it into `suites/` next to related tests with proper `meta.Begin` tags — see `.agents/docs/e2e-policy.md`.

### Inventory

| Category | Oracle | Pri | Coverage | Issue |
|----------|--------|-----|----------|-------|
| smoke | login / pad tele / relog / world alive | P0 | covered (`TestSmoke_*`) | — |
| combat/charm | apply/cancel aura | P1 | covered; Yogg MC logout `blocked-harness` (no charm/MC drive) | #25506 |
| combat/death | die → ghost → release → reclaim | P1 | covered | — |
| combat/pets | summon / GUID / attack / dismiss | P1 | covered; dungeon Raise Dead `blocked-harness` (ready-check / instance summon) | #27081 |
| combat/threat | engage / taunt switch / kill clears combat | P1 | covered | — |
| combat/vehicles | spellclick steed enter/exit | P2 | covered | — |
| spells/aura | apply/query; CC broken by damage; mount persist | P1 | covered (`TestAC_26130_*`) | #26130 |
| spells/cast | Charge on dummy; fail path; stance; Raise Dead + ghoul | P1 | covered (`TestAC_27061_*`) | #27061 |
| spells/effects | Charge / grounding totem / Sweeping Strikes Execute | P1 | covered (`TestAC_26997_*`); dummy-summon `blocked-harness` (engineering dummy lifetime) | #26774 #26997 |
| social/group | form / leave / leader / loot method / disband | P2 | covered | — |
| social/loot | need/greed / master loot; below-half kill | P1 | covered (`TestAC_26862_*`); chest mid-roll `blocked-harness` (GO 194821 UseGameObject); pass-on-loot delete `blocked-harness` (item-survive after ALL_PASSED) | #26894 #26862 #22000 |
| social/trade | item+gold accept; cancel; walk-OOR TARGET_TO_FAR | P1 | covered | #25723 |
| quests/lifecycle | STAY_ALIVE fail on death; status after save/relog | P1 | covered (`TestAC_26549_*`) | #26549 |
| quests/escort | find spawned unit; follow-NPC despawns on logout | P2 | covered (`TestAC_24450_*`) | #24450 |
| items/equip | visible-item slot after EquipEntry; additem; survives relog | P2 | covered | — |
| protocol/session | pos; item/quest load; money save/relog | P1 | covered; GM vis persist `blocked-harness` (extra_flags after relog) | #25793 |
| protocol/teleport | cross-map; named; GoCreatureID | P1 | covered | — |
| guild/charter_bank | charter buy+turn-in | P2 | covered | — |
| instances/bind_reset | party tele; ritual summon | P2 | covered; post-reset summon `blocked-harness` (AcceptSummon after reset) | #10708 |
| instances/classic/stratholme | Timmy remains hidden while a relevant Square Scarlet lives, then emerges after the area is clear | P2 | covered (`TestAC_26363_TimmyEmergesAfterSquareCleared`) | #26363 |
| instances/ulduar | named tele; Freya wave interval | P2 | covered (`TestAC_27095_*`); Kologarn Charge `blocked-harness` (bridge Z after Charge) | #26266 #27095 |

---

## Parallelism and isolation

### Model

| Layer | Behaviour |
|-------|-----------|
| **Packages** | `go test -p N` runs packages concurrently. Default **`P=1`** in the Makefile — IsolationPads has 27 unique pads; more packages than pads **hash-share** and can thrash. Raise `P` only when every concurrent package has a unique preferred pad. |
| **Tests in a package** | `meta.Begin` is **serial by default** (no `t.Parallel`). Tag `parallel` only if pad-safe. Makefile `PARALLEL=1`. |
| **Pads** | `e2eharness.PackagePad(t)` is **sticky per suite folder** for the process lifetime. |

Use **`PackagePad`** for combat/social placement — not a shared Stormwind cell for every suite.

```go
pad := e2eharness.PackagePad(t)
bot.TeleportPad(t, pad)
// multi-bot:
e2eharness.FormPartyAtPad(t, pad, leader, mate)
e2eharness.TeleportAllPad(t, bots, pad)
```

`PadStormwindOutskirts` is a **legacy alias** of the AbandonHouse pad. Prefer `PackagePad(t)`.

### Isolation pads

Far-apart world locations (operator-captured). Combat-heavy packages have preferred 1:1 assignments; other packages take free pads or a stable hash share when the pool is exhausted.

| Pad name | Map | Notes |
|----------|-----|--------|
| `Tower1` | Eastern Kingdoms (0) | Preferred: `combat/threat` |
| `Tower2` | Eastern Kingdoms (0) | Preferred: `combat/death` |
| `AbandonHouse` | Eastern Kingdoms (0) | Preferred: `combat/pets`; legacy SW outskirts alias |
| `NagrandArena` | Outland (530) | Preferred: `combat/charm` |
| `FloatingIsland1` | Outland (530) | Preferred: `combat/vehicles` |
| `FloatingIsland2` | Outland (530) | Preferred: `social/loot` |
| `FloatingIsland3` | Outland (530) | Preferred: `social/group` |
| `InMountains1` | Kalimdor (1) | Preferred: `social/trade` |
| `InMountains2` | Kalimdor (1) | Preferred: `spells/cast` |
| `InMountains3` | Kalimdor (1) | Preferred: `spells/effects` |
| `ElwynnRidge` | Eastern Kingdoms (0) | Elwynn cliff |
| `BurningSteppes` | Eastern Kingdoms (0) | ~300y from ElwynnRidge |
| `Mulgore1` | Kalimdor (1) | Preferred: `quests/escort` (plains, spawn-safe) |
| `Mulgore3`–`Mulgore5`, `MulgoreNorth` | Kalimdor (1) | Mulgore / Thunder Bluff mesa |
| `Boulderslide` | Kalimdor (1) | Stonetalon Boulderslide Ravine |
| `Stonetalon1`, `Stonetalon2` | Kalimdor (1) | Stonetalon Mountains |
| `Talondeep` | Kalimdor (1) | Talondeep Path (near Ashenvale1) |
| `Ashenvale1` | Kalimdor (1) | Ashenvale |
| `FelwoodSouth`, `MorlosAran` | Kalimdor (1) | Felwood |
| `HyjalApproach`, `Hyjal1`, `Hyjal2` | Kalimdor (1) | Hyjal |

Unlisted suites (`smoke`, `spells/aura`, protocol, quests, items, instances, guild, …) receive the first free pad, then hash-share if needed. Logs include `PackagePad suite=… pad=…`.

**Do not** hardcode one shared coordinate for every new combat test. Content that *must* use a fixed world location (tabard designer, instance entrance, named tele) still may — then return to the package pad when possible.

---

## Cleanup rules

Live e2e mutates a real realm. Cleanup is mandatory.

### Persistent spawns (`.npc add` / `.gobject add`)

These write **DB rows**. Bare add without cleanup litters pads (e.g. Crimson Templar 15209, Gift of the Observer GO 194821).

| Do | Do not |
|----|--------|
| `bot.Spawn` / `SpawnKillLootable` / `SpawnGameObject` (register cleanup) | Bare `.npc add` / `.gobject add` |
| `DespawnCreatureSpawn` / `DespawnGameObjectSpawn` by **DB spawn id** | Assume process exit cleans world DB |
| Rely on SQL DELETE + soft live despawn (harness does both) | Only live delete after socket already closed |

Despawn path: **SQL DELETE always** (survives session close) + optional soft `.npc delete` / `.gobject delete` while the socket is open.

**Loot:** do not use `.npc add temp` for loot tests (`TEMPSUMMON_CORPSE_DESPAWN` removes the corpse; use `Spawn` / `SpawnKillLootable`).

### Pets, ghouls, guardians, totems

`NewScenario` / `NewSolo` register `CleanupOwnedSummons` on `t.Cleanup` (dismiss pet + despawn units with SUMMONEDBY/CREATEDBY = player).

After Raise Dead / heavy summon use, also call while still InWorld:

```go
bot.CleanupOwnedSummons(t)
```

### SoftPass (fail-closed)

`SoftPass` / `SoftPassf` **fail the test by default** so unjudgeable fixtures cannot greenwash CI. Only local debug:

```bash
E2E_ALLOW_SOFT_PASS=1 go test -tags=e2e …
```

Prefer `Preconditionf` (setup blocked), `Assertf` / `ConfirmedBugf` (oracle), or `HarnessFailf` (infra).

---

## Writing a test (short)

```go
//go:build e2e

package pets_test

import (
	"testing"

	_ "github.com/go-sql-driver/mysql"

	"github.com/azerothcore/azerothcore-wotlk/e2e/internal/meta"
	"github.com/azerothcore/AzerothGhost/e2e/e2eharness"
)

func TestPets_Example(t *testing.T) {
	meta.Begin(t, meta.TestMeta{
		Tags:     []string{"med", "combat", "pets"},
		Runtime:  "med",
		Category: "combat/pets",
	})

	bot := e2eharness.NewSolo(t, e2eharness.ScenarioOpts{
		Prefix: "PetEx",
		Class:  e2eharness.ClassWarlock,
		Level:  80,
		LearnAllClass: true,
	})
	bot.TeleportPad(t, e2eharness.PackagePad(t))
	// setup (GM ok) → CombatReady before pulls → drive → assert
	t.Logf("PASS …")
}
```

Checklist:

1. `//go:build e2e` + MySQL blank import + `e2eharness`.
2. `meta.Begin` (tag filters; **serial by default** — tag `parallel` only if pad-safe).
3. Unique short `Prefix` (**≤ 7 chars**: account = Prefix + 2 digits + 8 hex, auth max 17);
   place with **`PackagePad`**.
4. Flow: fixture → place → setup → `CombatReady` if pull → drive → assert.
5. Waiters (**Arm → Send → Wait**), not fixed long sleeps.
6. Severity helpers for fatals; quest DB only after `Save`.
7. Spawn helpers with cleanup; summons cleaned via `CleanupOwnedSummons`.

Full API surface: harness `LLM_GUIDE.md` / `EXAMPLES.md`.

---

## Common flakes and fixes

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| NPC ignores bot / no aggro | GM mode still on | `CombatReady` / `CombatReadyFull` before pull |
| Boss resets mid-fight | `.gm on` for `.damage` | `Damage` / `DamageKill` (mode off) |
| Melee out of range after `.tele` | Named tele short of target | `GoCreatureID` after `TeleNamed` |
| Unit not in cache after tele | Cache clear on transfer | Re-`WaitUnit` after tele / `WaitInWorld` |
| Random combat on pad | Leftover spawns / pets / other package thrash | Cleanup helpers; `-parallel 1`; PackagePad |
| Cast fails “in combat” on empty pad | Contested pad / leftover temp | `CombatStop`; despawn litter; unique pad |
| Loot roll never starts | Wrong creature or threshold | `CreatureGroupLootFixture` (15209) + `LootThresholdUncommon` |
| Quest status wrong in DB | Asserted before save | `Save` / `QuestStatusAfterSave` |
| GM commands silently ignored | Wrong race language | Set `Race` on bot (harness uses racial language) |
| `SOFT-PASS disabled` fatal | SoftPass without opt-in | Use real severity helpers, or `E2E_ALLOW_SOFT_PASS=1` locally only |
| `precondition:` spam | Stack/DB/env mismatch | Check `E2E_*`, world up, DSN DB names |
| Intermittent pass without code change | Sleeps / races | Replace sleeps with waiters; fix Arm→Send→Wait |

A test that fails intermittently on a **correct** core is a test/harness bug until proven otherwise. Do not hide flakes with longer sleeps or SoftPass.

---

## Severity markers

| Helper | Meaning |
|--------|---------|
| `Preconditionf` | Setup never reached a judgeable state |
| `ConfirmedBugf(t, issue, …)` | Core wrong for tracked AC issue/PR |
| `HarnessFailf` | Infra: timeout, SQL, empty cache, send error |
| `Assertf` / `AssertBugf` | Post-drive product oracle |
| `SoftWarnf` | Non-fatal note |
| `SoftPass` | **Fail-closed** unless `E2E_ALLOW_SOFT_PASS=1` |

---

## Policy (when to add e2e)

Use `.agents/docs/e2e-policy.md` for decision trees (e2e vs unit, mandatory triggers, MVT). This README is **how to run and structure** the suite; the harness guides are **how to author** scenarios.

---

## CI

Details live in the workflow files only:
[`.github/workflows/e2e-live.yml`](../.github/workflows/e2e-live.yml),
[`.github/workflows/core-build-nopch.yml`](../.github/workflows/core-build-nopch.yml).

| How | Effect |
|-----|--------|
| Non-draft PR in **azerothcore/azerothcore-wotlk** | nopch `ubuntu-24.04`/clang-18 compiles + dry-run, then **full** e2e reuses those binaries |
| Merge to `master` | same clang-18 nopch build, then **full** e2e again (flake + merge-base drift) |
| Actions → **e2e-live** → Run workflow (official repo; needs workflow on default branch, or `gh workflow run … --ref e2e`) | Compiles on the runner; choose scope (smoke/full) |

Day-to-day development and agent debugging should use a **local** stack + `e2e/local/` or the committed suites — not CI setup docs.

Greppable failure prefixes: `precondition:`, `AC#N CONFIRMED BUG:`, `harness:`, `WARNING:`.

### Observatory population coverage

P2, covered via `apps/observatory/tests/population_live.py`: authenticated controls drive actual bot logins/logouts,
paused resize, 1→3→1→0→3 and active AI without humans. AzerothGhost coverage is blocked by its lack of an observatory
HTTP-control/telemetry client and by the deliberate rejection of human world sessions in simulation mode. The
committed Python live-stack driver provides the feature oracle; frontend and bridge validation cover form/parser logic.

### Observatory GM POV coverage

P1, covered by `suites/observatory/TestObservatory_GmPovConnectionLock`: native GM-only admission, POV state,
read-only actions, connection-lifetime 1× locking, multi-observer and abrupt-disconnect cleanup. Requires an exclusive
observatory run; opt in using `E2E_OBSERVATORY_URL`, `E2E_OBSERVATORY_TOKEN_FILE` and `E2E_OBSERVATORY_SPOOL`.
See [the validation instructions](../apps/observatory/docs/VALIDATION.md#gm-only-native-pov-exclusive-disposable-run).
Without that opt-in the special-realm test is excluded from ordinary realm suites. Addon visual rendering and
cinematic exit remain manual-only. The pinned harness OS FourCC issue is handled in test account setup, without
changing the harness pin or disabling Warden.

### alles memory coverage

P1, source-only, uncompiled and unrun: the `brain` variant of `TestAlles_ExplorationAcquiresWork` requires
Alles body ownership, route advancement, client-visible ordinary travel and a saved newly acquired quest.
Its optional `travelStarted` fixture starts at the committed journey; `combatInterruption` additionally requires
combat followed by resumption of that same travel intention. See `suites/alles/README.md`. Live acceptance and
body-mode regression runs for reward/preparation/cooperation/handoff remain pending.

P1, compiled but not run: `suites/alles/TestAlles_ExplorationAcquiresWork` observes local investigation,
deferral, ordinary travel and an actual saved newly accepted quest. `TestAlles_ObjectiveEarnsQuestReward`
requires new quest credit and an authoritative saved reward. Both require an exclusive disposable objective
cohort and `E2E_ALLES_OBJECTIVE_FIXTURE`; see [fixture requirements](suites/alles/README.md).
`TestAlles_QuestReadinessDefersWithoutAbandoning` uses an observed level change on that exact disposable target
to require bounded strength deferral, another active intention and saved quest retention without reward. It also
compiles but has not run live. Objective relog/handoff and live repair/respawn competition remain gaps.

P1, covered: `suites/observatory/TestObservatory_NavigationRecoveryAndAreaSearch` passed on 2026-09-08 and
uses two native POV clients to require movement and completion of seeded quest/search intentions without repeated
search-to-travel reversals. It requires a fresh exclusive realm (under 60 simulated seconds), observer admission,
the standard `E2E_OBSERVATORY_*` settings, and `E2E_OBSERVATORY_NAVIGATION_FIXTURE` pointing to JSON with
`disposable: true` and two `bots: [{name, guid, quest, place}]`. Each bot must have intention ID 1 active in its
clean fixture: a ready-to-turn-in quest at the stuck location, or a search just outside its destination boundary.
Run with `-parallel 2` for the two read-only observers. This fixture intentionally changes initial state; use a
separate fresh realm for subsequent simulation. A passing run proves these routes, not all world navigation.

P1, compiled but not run: `suites/alles/TestAlles_HumanInvitationSuspendsAndResumesQuest` requires the exact normal
human-led party, acknowledged saved quest checkpoint/attempt, a paused active-work timer and autonomous resumption
after the human leaves. It uses the exclusive objective fixture on a quiet route and cleans up the new party.
Real logout/cache-eviction/relogin persistence and live human handoff remain unverified.

P1, compiled but not run: `suites/alles/TestAlles_ObjectiveSurvivesEvictionAndClientRelog` uses explicit disposable
client takeover, acknowledged graceful logout, owner eviction and a new generation. A witnessed post-relog SAY
must force a newer snapshot containing that input and the exact retained quest/private-knowledge payload.
Requires the suite's Human Warrior takeover fixture and no automatic bot relog during the eviction window.
Autonomous bot resumption after relog remains a live acceptance gap; this test retains client control.

P1, compiled but not run: `suites/alles/TestAlles_ObjectiveEarnsQuestReward` has a `fundingQuest` variant.
An active paying quest must gain real credit and its saved reward, produce an observed sufficient own balance,
and lead to the retained money-blocked parent's actual saved reward. Only the observer is teleported; no funds or credit
are granted. See the suite's paired quest and separated turn-in fixture requirements. Live income acceptance remains pending.

Deterministic Alles trial support: `apps/alles/cmd/alles-fixture-provider` returns bounded, journaled responses
through the ordinary worker provider adapter, with exact delivered-context matches and unchanged worker/world
validation. Its memory/conversation/planning adapter and rejection tests pass offline with the race detector.
The example is a starting point; case-specific advice/cooperation rules and live keeper results remain pending.
See `apps/alles/README.md`. Keep scratch scenarios and orchestration under `e2e/local/` and retain trial journals.

P1, compiled but not run: `suites/alles/TestAlles_EquipmentPreparationPaysForRealRepair` requires client-observed
approach, actual saved equipped durability gain and matching currency expense. Its `recallRepair` fixture variant
starts beyond visible-service search range, requires a saved personally observed repair site and returning telemetry,
then the same real paid repair. Both variants remain unrun and use the exclusive objective fixture with already
damaged equipment and sufficient own funds. Live party preparation and discovery of never-encountered services
remain gaps.

P1, compiled but not run: `suites/alles/TestAlles_SupplyPreparationBuysRequiredItems` requires actual new quest
items and matching saved own-money expense after normal merchant purchase. Its exclusive disposable fixture
freezes the target during setup and creates a guarded temporary merchant, then unfreezes for normal execution.
Its optional `fundingItem` fixture starts with zero money and requires the exact owned junk stack to shrink plus
positive sale income and the matching saved net balance. See the suite's quest/item/vendor, funds and quiet-scene
requirements. Live supplies acquisition, earning missing money, distant vendor discovery and coordinated party
preparation remain gaps.

P1, compiled but not run: `suites/alles/TestAlles_HeardMemorySurvivesRelog` asserts ordinary heard text becomes
locally attributed hearsay, a same-map distant receiver retains nothing, and the listener's memory survives an
acknowledged flush, cache eviction and login. It requires an exclusive configured mod-alles realm and explicit
`E2E_ALLES_FIXTURE` opt-in. See [the fixture and fake/fallback runs](suites/alles/README.md). The six-actor watched
death-to-rumor scene remains manual-only; bot/human takeover, foreign-language and hidden-killer live coverage
remain gaps.

P1, covered: `suites/alles/TestAlles_OnlineHumanAudience` passed on 2026-09-07. It verifies privileged-account
local SAY/YELL capture and autonomous bot SAY reaching a nearby unmanaged human with no other configured
audience nearby. Requires `E2E_ALLES_AUDIENCE_FIXTURE`; see the same suite guide. This does not assert an LLM
conversation or HELP action.

P1, covered by keeper: the earlier `suites/alles/TestAlles_NaturalConversationAndFollow` passed on 2026-09-07
with the Ollama provider. The revised request-objective version awaits a fresh live run. It verifies natural
SAY/YELL, separate addressed respondents, short history, physical follow through client movement, stop, and
combat assistance against an engaged NPC. Requires `E2E_ALLES_CONVERSATION_FIXTURE` and permission to summon
and recall its two bots; see the suite guide. Language interpretation remains model-dependent.

P2, conversation telemetry: `suites/alles/TestAlles_ConversationJournal` drives actual SAY/YELL and checks
matching delivered replies and action results in the live journal, including actual wave packets to the human.
The revised wave oracle awaits a fresh live run. It reuses `E2E_ALLES_AUDIENCE_FIXTURE` and
moves only the disposable human; see the suite guide for conversation and journal prerequisites.

P1, satisfaction activity keeper: `suites/observatory/TestObservatory_SatisfactionProducesObservedRest` requires
a fresh exclusive Observatory realm with a managed bot and native GM POV enabled. The offline fixture seeds
that owner with rest fulfillment 0.1, rest weight 10, other weights zero, default effects and no previous activity
receipts. Set `E2E_OBSERVATORY_SATISFACTION_FIXTURE` to a private JSON file containing `disposable: true`, `name`
and `guid`, plus the standard Observatory URL/token and `E2E_*` database settings. Start within 30 simulated
seconds of boot. The client observes stationary rest, telemetry verifies 60 seconds of observed recovery, and
the database oracle waits for its durable anti-replay receipt. Run alone with `-timeout=3m`; compilation alone
does not establish a live pass.

The companion variant `TestObservatory_SatisfactionSocialTravelAndReload` uses two fresh Human bots. Seed a
recent personally observed `Humanb` meeting site 65 yards from `Humana`, keep the companion's rest-focused
profile, and add `companion` and `companionGUID` to the fixture JSON. Run after the rest keeper on the same
exclusive realm with `-run '^TestObservatory_Satisfaction' -timeout=7m`. GM setup changes only the source's
motive weights. The client must observe travel and an ordinary greeting; population logout/rejoin must preserve
the source's social receipt and fulfillment. The test restores observer permissions afterward.


### Individual motivations and outcome learning

`TestObservatory_IndividualMotivationsLearnObservedOutcomes` requires an exclusive fresh Observatory cohort
with at least two newly initialized Alles owners and GM observers enabled in locked mode. Set
`E2E_OBSERVATORY_MOTIVATIONS_FIXTURE` to private JSON containing
`{"Disposable":true,"Name":"<watched bot>","GUID":123}` and the normal `E2E_*` DB/Observatory settings.
Run `go test -tags=e2e ./suites/observatory -run TestObservatory_IndividualMotivationsLearnObservedOutcomes
-count=1 -v -timeout 8m` on one line. The oracle requires native movement, distinct ambitions, actual carried
money, mastery increasing by actual earned XP, positive starter gear value, a measured attempt and committed
version 14 learning data. The observer never changes preferences or grants XP/money/equipment. Run this separately from throughput comparisons.

### Conversational memory quality

`TestObservatory_GreetingsDoNotBecomeRecursiveNews` reuses the exclusive disposable motivation fixture and
Observatory environment above. It temporarily enables full GM observation, moves only its disposable human
to the target, sends an ordinary greeting twice beyond the ingress deduplication window, and requires one
committed low-salience reflex memory with the same identity and no recursive greeting SAY over 75 game seconds.
It restores observer mode and revokes its test GM access. Run separately from timing comparisons with
`-run TestObservatory_GreetingsDoNotBecomeRecursiveNews -count=1 -v -timeout=4m`.
