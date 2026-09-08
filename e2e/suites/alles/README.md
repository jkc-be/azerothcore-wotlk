# alles live persistence fixture

`TestAlles_HeardMemorySurvivesRelog` has been compiled but has not been executed. It exercises the
ordinary client SAY, self recall, explicit module flush acknowledgment, cache eviction and login paths. Its
primary oracle is a locally attributed heard memory surviving an actual reload. An actor moved to another
existing pad on the same map must receive neither the challenged SAY nor its memory/pending perception.

The six-actor Northshire death-to-rumor scene and human assessment remain separate first-light gates. This test
does not exercise autonomous playerbot speech or takeover, hidden-killer visibility, foreign-language capture,
NPC hearing, provider interpretation, forced DB failures or a hard shutdown deadline.

## Exclusive setup

Build/configure and live-stack changes require explicit authorization under the repository's AGENTS.md.
Use the native host auth/world/MySQL stack and disposable fixture data. The suite opts in only when
`E2E_ALLES_FIXTURE` names a manifest; it is skipped by ordinary suites without that opt-in.

1. Provision three disposable accounts, each owning exactly one Human Warrior created through the client protocol.
   Account names must match `ALLESE2E[A-Z0-9]{1,9}`, use the harness password `test`, and have GM level 3 for setup
   and processing diagnostics. Record the actual account, character name and character GUID for each role. Keep
   these accounts out of the random-playerbot population and leave all three characters offline before the run.
   Explicitly deny RBAC permission 372 (`RBAC_PERM_COMMAND_GM_CHAT`) for the speaker account at realm `-1`, with
   no conflicting grant. Core selects the GM chat opcode from this permission, independently of `.gm chat off`.
   The source fixture needs the ordinary player SAY opcode while retaining GM security for setup/diagnostics.
   The suite verifies this permission fixture and does not change it.
2. Copy [fixture.example.json](fixture.example.json) to a local file under `e2e/local/`, replacing all example
   identities with those actual disposable characters. The suite verifies account ownership, exact name/GUID,
   race/class and offline state before any login. No character or account is created by this suite.
3. Start an exclusive ordinary 1x realm with static mod-alles and mod-playerbots, the module migration installed,
   and only those three GUIDs in `Alles.Owners`. Set `Alles.Enable=1`, `Alles.Worker.Mode=inprocess-fake`,
   `Alles.SchedulingProfile=pilot`, `Observatory.Enable=0`, `Alles.Memory.HalfLifeSeconds=86400`,
   `Alles.Memory.HearsayCap="0.6"`, and ordinary SAY range 25. Retain the default memory and perception caps.
   There must be one realm in the auth realm list. Use an exclusive realm because the test uses two existing pads
   and changes the fixture characters' location, GM flags, god mode and saved memories.
4. Set the normal `E2E_AUTH_ADDR`, `E2E_AUTH_DSN`, `E2E_CHAR_DSN`, and `E2E_WORLD_DSN` to that stack. Set
   `E2E_ALLES_FIXTURE` to the manifest's absolute path. No schema or module config reload is attempted by the test.

The startup owner allowlist is immutable. `NewScenario` cannot create random GUIDs and then enroll them after
startup, so the fixture uses the pinned client's SRP/character-enum/login calls and wraps the resulting session
in `ScenarioBot`. It never falls back to a different character or creates a missing one. The pinned AzerothGhost
v1.0.8 OS FourCC issue is handled after SRP by setting `account.os='Win'` for that validated fixture account only,
as in the existing Observatory consumer test. Warden remains enabled. The fixture retains its characters and
memory rows for inspection; session cleanup logs them out.

## Runs and evidence

From `e2e/`, after build/toolchain/setup authorization:

```sh
go test -tags=e2e ./suites/alles -run '^TestAlles_HeardMemorySurvivesRelog$' \
  -count=1 -v -timeout=10m -parallel=1
```

For the default fake run, set `Alles.Worker.FakeWithhold=0` and manifest `formation` to `fake`. The retained
challenged memory must have `FormationMode::InProcessFake`; an accidental fallback does not satisfy this run.
Repeat with `-count=2` after the first successful run to check fixture stability.

For the separate expiry run, restart with `Alles.Worker.FakeWithhold=1` and set manifest `formation` to `fallback`.
The same client path must complete with `FormationMode::Fallback`. Permit/lease expiry may precede the absolute
45-real-second admission deadline. This run establishes bounded eventual fallback, not which expiry fired;
the coordinator fixtures distinguish those boundaries. It does not substitute for a provider timeout test.

Both receivers first hear a unique warmup line while nearby. This establishes functional reception and a real
nonzero revision before the distant receiver moves. The challenged line has another per-run marker. Waiters
are armed before sends, and system-message command barriers cannot be acknowledged by ordinary SAY packets.
Assertions check typed source, heard kind, reported attribution, local confidence cap and formation mode, rather
than comparing a complete generated sentence. The marker selects only this run's memory.

Before each memory/perception SQL oracle, the fixture sends core `.save`, requests `.alles flush`, waits for
`committed_revision >= requested_revision`, then verifies the watermark in `alles_actor`. Negative coverage reads
both pending and formed state so interpretation delay cannot hide a leaked observation. The listener logs out,
another session waits for its owner to become `unloaded`, and a new connection logs in with the same GUID. The
new store generation, self recall and unchanged persistent memory identity/provenance must all agree.

The persistence fixture's fake and fallback runs remain unverified. The later live results below cover separate
audience and conversation tests; they do not establish persistence across relog.

## Unmanaged online audience and privileged local chat

`TestAlles_OnlineHumanAudience` passed on the native ordinary realm on 2026-09-07 (30.08 seconds).
It sends normal SAY and YELL from a disposable account with default GM permissions and `.gm chat off`, checks
both inputs after an acknowledged module flush, and observes autonomous bot SAY on the unmanaged human's socket.
The human must have no `alles_actor` row. Fresh world telemetry must place all other configured owners more than
60 yards from the speaker, or their characters must be confirmed offline. The test follows the autonomous bot
using only the disposable observer; it does not change the bot's movement, strategy, memory or account.

Opt in with `E2E_ALLES_AUDIENCE_FIXTURE` pointing to a JSON manifest with this shape (replace identities/paths):

```json
{
  "version": 1,
  "disposable": true,
  "actor": {"role": "audience", "account": "ALLESE2EAUDIT", "name": "Allesaudit", "guid": 1428},
  "target": {"name": "Humane", "guid": 1216},
  "otherOwners": [1176, 1186, 1196, 1206, 1226],
  "telemetry": "/absolute/path/to/var/run/alles-telemetry/latest.json"
}
```

Use the same SRP fixture credentials and ownership checks described above. This account needs global GM level 3
with no RBAC 372 override, and must be absent from `Alles.Owners`. `otherOwners` must list every configured owner
except the target. Speech must be enabled with cooldown at most 30 seconds, and the isolated target must have
multiple eligible witnessed-death/heard-statement memories. Keep the ordinary 25-yard SAY range. Normal
`E2E_*` connection settings apply. Run only this test with `-run '^TestAlles_OnlineHumanAudience$' -timeout=2m`.

This test does not claim an LLM reply to HELP, model dispatch, or a gameplay help action. The successful local
run used the exhausted 100-request ledger and existing/fallback memories. An initial stationary-observer attempt
failed after the target moved away; the maintained test keeps its observer nearby. Logs are retained locally.

## Objective execution and exploration

`TestAlles_ExplorationAcquiresWork` and `TestAlles_ObjectiveEarnsQuestReward` are compiled acceptance tests;
they have not yet run against the changed worldserver. They use `E2E_ALLES_OBJECTIVE_FIXTURE` with version 1,
`disposable=true`, the existing disposable Human Warrior `actor` identity, one online `target` identity
(`RNDBOT...` account, exact name/GUID), and an absolute `telemetry` path. No accounts or bots are created by these
tests. The target must be an autonomous configured Alles owner with `Alles.Objectives.Enable=1`, New RPG active,
and the planning migration installed on an exclusive ordinary realm. Disable random teleport/level resets.

For exploration, supply `origin` and `destination` area IDs. Start the target investigating exhausted local
work in `origin`, with a suitable privately known destination and no outstanding viable quests. The test observes
repeated spatial searches, deferral, a new travel intention, client-visible movement and arrival, then requires
the newly accepted quest in the character database after `.saveall`. Map jumps and movement beyond ordinary
travel speed fail the test. Use separate fixtures for different starting profiles; there is no test-side route.

For quest execution, supply `quest` and start the bot on that accepted, incomplete combat/loot quest. The test
requires additional measured credit, objective completion and an actual saved quest reward. A claimed completion
or chat line cannot pass either test. The observer alone teleports to remain nearby; neither test supplies quest
credit, grants rewards, moves the target, or changes its strategy. `.saveall` saves the exclusive realm's players.

Run each test separately with `-tags=e2e -count=1 -parallel=1 -timeout=12m`; their setup requirements differ.
Relog/handoff and remote advice still need live acceptance coverage.

For `TestAlles_ObjectiveEarnsQuestReward`, optional `fundingQuest` selects an accepted paying quest as the active
work while `quest` names a distinct retained turn-in blocked only by insufficient money. Begin with
`earning_quest_money`, a positive-money source quest still requiring credit, the negative-money parent deferred
for supplies, and actual balance below the parent's payment. Disable unrelated commerce, currency grants and
cheats; the source's normal work/reward must provide enough money. Keep the two turn-in locations far enough
apart to observe the funded balance before payment. The test requires new source credit and its saved reward,
an observed sufficient actual balance, then the retained parent's completed intention and saved reward. It never
grants money, items, credit or rewards. Run alone with `-timeout=15m`; the original single-quest variant is unchanged.
The funding variant compiles only and has not run live.

`TestAlles_HumanInvitationSuspendsAndResumesQuest` uses the same objective fixture with `quest`. Begin with an
ungrouped target actively traveling for an accepted incomplete quest, healthy equipment, and a quiet route away
from combat, credit triggers and turn-ins. The disposable human must be eligible to invite the same-faction bot.
The test sends a normal party invitation and requires the exact two-member ordinary party on the human's client.
The bot must yield to human control, save the same quest checkpoint/attempt, stop its active-work timer while the
human leads, and resume the retained attempt after the human leaves. Module flush watermarks acknowledge the
planning snapshots; the normal quest must also remain saved or have its actual reward. The observer alone moves;
cleanup leaves the newly formed party. Run alone with `-timeout=3m`. This keeper compiles only; it has not run live
and does not establish logout/cache-eviction/relogin persistence.

`TestAlles_ObjectiveSurvivesEvictionAndClientRelog` adds a separate persistence oracle using the same quiet quest
fixture with `clientTakeover: true`. The exact target must be an ungrouped Human Warrior on its disposable numbered
RNDBOT account, already provisioned with the harness password; offline sibling characters are allowed, online
siblings are refused. Provision the cohort with automatic bot relog disabled for the test window and client self-bot
AI disabled. Use the deterministic fallback configuration with conversation disabled. The test does not change
credentials or population settings. Its normal client login takes over the exact bot through SecureLogin, then
requires a suspended saved quest with unchanged credit/attempt and nonempty private place knowledge. Graceful
logout must produce `SMSG_LOGOUT_COMPLETE`, an unloaded owner and a fresh generation on the next client login.
A witnessed normal SAY after relog must create a newer acknowledged snapshot containing that specific input;
the complete planning payload must match the saved baseline. This prevents an old database row from passing
without a fresh load and save. Only the observer teleports. Target clients log out during cleanup. Run alone with
`-timeout=3m`. This keeper compiles but has not run live; it covers persistence under client control, while actual
autonomous bot resumption after relog remains a separate live acceptance gap.

`TestAlles_QuestReadinessDefersWithoutAbandoning` uses the same fixture with `quest`: a normal solo quest of
level 5 or above, incomplete and currently active in the travel step. Begin out of combat on a safe route with
healthy equipment, within the executor's level range, with another feasible quest or local investigation available.
This test lowers only the exact disposable target to level 1 through the normal GM setup command, verifies the
client receives that level, then requires a strength deferral held for 30 seconds without another attempt, a
different active intention, and the original quest retained without reward after saving. Cleanup restores and
saves the original level. The GM command resets level XP and may change talents; this fixture is disposable,
not a way to test an existing player's character. Run alone with `-timeout=2m`. The keeper compiles but has not
run against the changed worldserver. It does not cover actual repair, supplies purchasing or respawn competition.

`TestAlles_EquipmentPreparationPaysForRealRepair` extends the fixture with `repairer` (full visible NPC GUID)
and `repairItem` (the low GUID of an equipped item, not its template entry). Begin active solo repair preparation
15-60 yards from that stationary friendly repairer, with already saved critical equipment durability and sufficient
own funds. Keep the route quiet, disable cheats and unrelated commerce/loot, and leave enough distance/time to
observe the initial damaged state. The test observes ordinary approach and preparation completion, then requires
the same still-equipped item to gain saved durability and the actual saved money decrease to match the recorded
expense. It never alters target durability, money or location. The observer alone teleports; `.saveall` affects the
exclusive realm. Run alone with `-timeout=3m`. The keeper compiles but has not run live; it does not prove repairs
for a party, supplies purchasing or acquiring missing money.

Set `recallRepair: true` for the same keeper's distant-return variant. First let the target personally encounter
an interactable repairer through ordinary gameplay, then stage critical gear on a quiet route 90-450 yards from
that remembered site. The target must have exactly one saved repair location on its current map; do not inject
planning knowledge. Keep the same friendly repairer there and the remaining two-minute attempt long enough for
the return. The test acknowledges the saved personal location, moves only the observer to inspect the service,
then follows the target. It additionally requires `returning_to_known_repairer` and an approach from beyond the
60-yard visible-service search range before the same real durability/payment oracle. This variant compiles only;
no live return journey has been run.

`TestAlles_SupplyPreparationBuysRequiredItems` extends the objective fixture with `supplyItem` and `supplyVendor`
template entries. Start the exact disposable solo target on an active incomplete quest requiring that item, with
zero owned copies, healthy equipment, sufficient own funds and bag space. The compatible friendly vendor must
normally sell the item for money without an extended cost. Keep the scene free of other vendors, combat, unrelated
commerce/loot and immediate turn-ins that could consume the purchased items or change the currency baseline.
The test freezes the target during setup, acknowledges a saved inventory/currency baseline, and spawns the merchant
only after checking that there are no existing nearby spawns of that entry. Cleanup removes the fixture spawn
and unfreezes the target. After unfreezing, normal bot execution must complete the purchase, gain real quest credit,
and save the required items with the exact recorded own-money expense. Only the observer teleports; no target items
or money are granted. Run alone on the exclusive disposable realm with `-timeout=3m`. The keeper has compiled but
has not run live; distant vendor discovery and party preparation remain gaps.

For the same keeper's funding variant, add `fundingItem`, the low GUID of the target's sole expendable junk stack,
and start with zero money. The stack must already be saved in carried inventory, be poor-quality miscellaneous
non-equipment without uses or quest relevance, and have enough vendor value to fund the required purchase. The
test grants nothing: it additionally requires that exact stack to shrink, positive recorded sale income, and a
saved balance equal to initial money plus income minus expense. Run the cash-funded and junk-funded fixtures
separately. Both variants have compiled only; live earning of missing funds remains unverified.

`TestAlles_CooperationGathersAndRewardsEachMember` extends the objective fixture with `companions`, an array of
1–4 exact disposable RNDBOT account/name/GUID records. The `target` is the intended party leader and `quest` is
an accepted ordinary group combat/loot quest. Begin with every member ungrouped and before recruitment or reward;
at least one companion must be 100 yards from the leader. Enable objective conversation and a grounded worker
fixture or provider. The bot cohort performs all recruitment, acceptance, movement, combat and rewards itself.

The test requires a real ordinary party record for every member, client-visible gathering, movement samples
within ordinary speed bounds, new credit for each participant, collective completion, and every individual's
saved reward after `.saveall`. It moves only the invisible observer and never supplies credit or invites bots.
Run alone with `-run '^TestAlles_CooperationGathersAndRewardsEachMember$' -timeout=15m -parallel=1` on an exclusive
disposable realm; reset the disposable cohort between runs. The test has compiled but has **not run live**.
Larger-party recruitment/roster coordination remains unfinished, so compilation is not a claim of passing behavior.

## Natural conversation and gameplay actions

The earlier `TestAlles_NaturalConversationAndFollow` passed on the native ordinary realm on 2026-09-07
(26.68 seconds), using the Ollama provider on `fedora-gpu`. The revised test addresses bots separately under
the bounded reply policy and moves the human with ordinary movement packets. It verifies natural SAY/YELL,
short conversation history, direct address selecting one respondent, physical following, stopping, and a real bot
attack-start packet against a temporary NPC already fighting the speaker. It does not establish arbitrary
language understanding, healing, quest interaction, or persistence of conversation history. This revised
objective-backed request path has compiled but still requires a fresh live run, including relog and handoff trials.

Opt in with `E2E_ALLES_CONVERSATION_FIXTURE` pointing to a private JSON manifest:

```json
{
  "version": 1,
  "disposable": true,
  "actor": {"role": "audience", "account": "ALLESE2EAUDIT", "name": "Allesaudit", "guid": 1428},
  "bots": [
    {"role": "bot", "account": "RNDBOT116", "name": "Humand", "guid": 1206},
    {"role": "bot", "account": "RNDBOT117", "name": "Humane", "guid": 1216}
  ],
  "telemetry": "/absolute/path/to/var/run/alles-telemetry/latest.json"
}
```

Replace all identities with validated fixture characters. The disposable human uses the SRP credentials and
global GM permissions described for the audience test and is excluded from `Alles.Owners`. The two bots must
be friendly, independent New RPG online owners with conversation enabled and an available provider budget. Normal
`E2E_*` connection settings apply. From `e2e/`, run:

```sh
go test -tags=e2e ./suites/alles -run '^TestAlles_NaturalConversationAndFollow$' \
  -count=1 -v -timeout=3m -parallel=1
```

This test moves the disposable human to Northshire, summons the two bots, waits for their client-visible
arrival, and uses recall during cleanup. It temporarily enables god mode for the human and spawns then deletes
one NPC. Run with authorization to move those bots and without other players depending on them. Test teleports
are setup commands; teleportation is not an available conversation action. Arena isolation is unsuitable because
FFA hostility prevents friendly follow actions. The earlier successful run returned the bots and logged the fixture out.

Provider wording is nondeterministic. Earlier attempts exposed fixture arrival races and a model shortening an
artificial nickname; a successful run with a natural nickname does not prove exact recall of arbitrary tokens.
Vague requests may still need better model clarification. Budget refill, durable charging, and rejection of
invalid or late actions have separate C++ coverage.

`TestAlles_ConversationJournal` reuses `E2E_ALLES_AUDIENCE_FIXTURE` to validate the delivery-to-journal path.
Enable conversation and telemetry journals and disable ambient speech. It moves only the disposable human to
the named existing bot, sends directly addressed SAY and YELL requests to wave, and matches each received reply
to its journal text, bot identity, human, channel, action result and job. Bot positions and strategies are not
changed by setup. Run it alone with `-run '^TestAlles_ConversationJournal$' -timeout=3m`.
