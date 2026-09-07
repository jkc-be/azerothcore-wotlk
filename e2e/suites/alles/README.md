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

## Natural conversation and gameplay actions

`TestAlles_NaturalConversationAndFollow` passed on the native ordinary realm on 2026-09-07 (26.68 seconds),
using the Ollama provider on `fedora-gpu`. It verifies natural SAY/YELL replies from both nearby bots, short
conversation history, direct address selecting one respondent, physical following, stopping, and a real bot
attack-start packet against a temporary NPC already fighting the speaker. It does not establish arbitrary
language understanding, healing, quest interaction, or persistence of conversation history.

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
be friendly, autonomous, online owners with conversation enabled and an available provider budget. Normal
`E2E_*` connection settings apply. From `e2e/`, run:

```sh
go test -tags=e2e ./suites/alles -run '^TestAlles_NaturalConversationAndFollow$' \
  -count=1 -v -timeout=3m -parallel=1
```

This test moves the disposable human to Northshire, summons the two bots, waits for their client-visible
arrival, and uses recall during cleanup. It temporarily enables god mode for the human and spawns then deletes
one NPC. Run with authorization to move those bots and without other players depending on them. Test teleports
are setup commands; teleportation is not an available conversation action. Arena isolation is unsuitable because
FFA hostility prevents friendly follow actions. The successful run returned the bots and logged the fixture out.

Provider wording is nondeterministic. Earlier attempts exposed fixture arrival races and a model shortening an
artificial nickname; a successful run with a natural nickname does not prove exact recall of arbitrary tokens.
Vague requests may still need better model clarification. Budget refill, durable charging, and rejection of
invalid or late actions have separate C++ coverage.

`TestAlles_ConversationJournal` reuses `E2E_ALLES_AUDIENCE_FIXTURE` to validate the delivery-to-journal path.
Enable conversation and telemetry journals and disable ambient speech. It moves only the disposable human to
the named existing bot, sends directly addressed SAY and YELL requests to wave, and matches each received reply
to its journal text, bot identity, human, channel, action result and job. Bot positions and strategies are not
changed by setup. Run it alone with `-run '^TestAlles_ConversationJournal$' -timeout=3m`.
