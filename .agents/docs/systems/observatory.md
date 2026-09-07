# Updating the server and Playerbot Observatory

Use `jkc-be/azerothcore-wotlk` `main` as the maintained source. The module revision is the Git submodule pin,
not whatever happens to be latest on module `master`. Read `apps/observatory/docs/HANDOFF.md`, `TIMING.md` and
`VALIDATION.md` before changing the simulation runtime. Preserve the native-client POV integration on ordinary servers.

## Agent update procedure

1. Establish the authorized machine and scope. Preparing a PR does not authorize deployment.
   A session instruction explicitly authorizing local/server updates takes precedence. Do not
   request authorization again for work already covered by that instruction. Record the checkout, install/config/data
   paths, service/container names, source revisions and whether `Observatory.Enable` is set. Never print credentials.
2. Inspect `git status --short`, `git submodule status` and module status. Preserve dirty work, remotes, local config,
   existing accounts and progress. Use a separate worktree if needed. Never force-reset a server checkout or overwrite
   another agent's changes. Fetch the same fork with `git fetch origin main`, review the incoming diff and pin the target
   commit. On a clean maintained checkout, use `git merge --ff-only origin/main`; resolve divergence in a review branch.
3. Inspect `.gitmodules` and any local module checkout before replacing it. An older deployment may have a manually
   applied POV patch. Preserve or migrate that patch and verify its behavior before changing the dependency checkout.
   After the module working tree is clean and its changes are accounted for, run `git submodule sync --recursive` and
   `git submodule update --init --recursive`. Never use `git submodule update --remote` during a routine deployment.
4. Build outside the source tree using the deployment's existing CMake options. Keep its enabled scripts/modules and
   custom script registrations. Run C++/frontend checks from the observatory README and the relevant unit tests. Run
   required live gameplay tests on an authorized disposable stack. Do not treat skipped CI jobs as passing builds.
   Record the exact tested core/module commits. Fix failures, commit the fixes, and repeat the affected checks.
5. Before a runtime update, finish or pause the current run and archive its manifest, initial snapshot, journal,
   snapshots, logs and measured results. Stop the exact managed process gracefully using its actual service/container
   manager. Preserve a rollback copy of installed binaries, effective configs and pre-update DB backups. Do not kill
   unrelated processes or restart a whole shared host. Install the verified binaries only after checks pass.
6. Update configuration by re-rendering it: `apps/observatory/render_config.py render` layers the new `.conf.dist`,
   the tracked example, the deployment's local overlay and its secrets file (see `docs/HANDOFF.md`); run it with
   `--check` against the current file first and review every listed difference. A deployment still edited by hand is
   adopted with `render_config.py derive`. Never copy an example over a complete runtime config, and never print or
   commit the secrets overlay. For a normal server with simulation disabled, preserve
   databases and use the normal updater. For an observatory run, **never resume a saved virtual-time database**: retain
   the export, restore all four `obs_` databases from the recorded clean pre-run fixture, and select a new run directory.
   Only disposable simulation databases may be reset. Existing production/human data is outside this reset procedure.
7. Start the world with the verified configuration, then start/repoint the bridge to the new spool and retain the
   private token file. Preserve the local extracted artwork directory and the bridge `--maps` argument;
   see `apps/observatory/docs/MAPS.md` for regenerating it after client data changes. Restarting the bridge drops
   Max speed to manual: re-select Max afterwards if the run was using it. With `Observatory.JournalSegmentBytes`
   set, the bridge prunes raw journal segments beyond `--retain-bytes`; the long-term files it writes into the run
   directory (`rollup`, `milestones`, `events-rollup`, `progression`) are then the only record of older history and
   must be archived with the run. Check startup logs, database
   version, module revision and the authoritative `/api/snapshot`.
   Verify a new run ID, advancing simulated time, complete cohort, increasing per-bot AI counters, pause/resume and
   requested versus achieved speed. Reconnect the browser and test an export. If updating ordinary POV, also test its
   watch/switch/stop and successful-action feed with simulation disabled. Keep GM observer admission disabled for
   timing comparisons and benchmarks. If explicitly enabling native GM POV,
   follow `INTERFACE.md`: verify ordinary-account rejection, connection-lifetime 1× lock and watch/stop/disconnect.
   Leave `Observatory.ObserverMode` at 0 (locked) for any run whose results will be compared; roam and full GM are
   exploration settings and the journal marks their use.
8. Report the deployed commit IDs, exact services changed, checks performed, dashboard address and any remaining
   correctness/performance limits. If validation fails, keep the new run stopped, retain failure artifacts and restore
   compatible previous binaries/configuration plus the appropriate pre-update DB state. A rollback never reuses virtual
   future timestamps as though a run were resumable. Never claim 100 bots at 10× without benchmark evidence.

## The dashboard on an ordinary realm (alles-live)

The same bridge and page serve `mod-alles` telemetry from the ordinary realm when `--spool` points at
`Alles.Telemetry.Directory`. On that feed the core's `Observatory::Event` records only reach the journal
through the module's live tap (`Observatory::SetLiveSink`), so a world build without it publishes no
`events.ndjson` and no per-bot AI counters: the dashboard then shows those figures as "not measured by this
world build" rather than empty panels. Check `/api/snapshot` for `journal` and per-bot `aiUpdates` to tell
the two cases apart, and start the bridge with `--worker-log env/dist/logs/alles-interpreter.log` so the
Interpreter panel can show the worker's own job outcomes. Every boot is a new run in the same directory;
the world archives the previous run's files into `archive/<run>/` and the bridge restarts its tiers.

## Diagnosing a dashboard stuck "reconnecting"

The bridge can be fully healthy while the dashboard never advances. `/api/snapshot` returning
`{"error": "Waiting for worldserver telemetry"}` means the bridge process itself is fine and worldserver simply
hasn't reported in — not a bridge problem. Before touching the bridge, check whether worldserver and authserver
are actually running (e.g. `pgrep -a worldserver authserver` in the process's actual host/container) and tail
their startup logs.

A common cause when the running binaries were built against a different worktree than the one the deployment
environment can see: `SourceDirectory` (the DBUpdater's SQL source) is set independently in `worldserver.conf`
and `authserver.conf` — the authserver config is often hand-maintained, not rendered through the shared template
— and can still point at the build worktree. If that path isn't visible where the servers actually run, both
processes fail during their automatic DB-update step ("DBUpdater: the given source directory does not exist,
… Shutting down") and exit cleanly before ever opening a listener; authserver's failure looks identical to
worldserver's. Fix by pointing `SourceDirectory` in both configs at a checkout the runtime can actually reach,
then relaunch.

Separately, `Observatory::Initialize()` refuses to reuse an existing `Observatory.Directory` (it treats
`create_directory` returning false — path already exists — as a hard refusal, logged as "Observatory
initialization refused"). A run directory left behind by an aborted launch means the next worldserver attempt
needs a brand-new run directory, with the bridge's `--spool` argument (and its service unit) updated to match
before restarting it — the same sequence a full restart performs.

## Making a pinned cohort actually play

`Observatory.BotGuids` only filters admission (`Observatory::AllowsBot`); whether an admitted bot
then plays is decided by the module. `RandomPlayerbotMgr::IsRandomBot()` requires the character's
account to be in `PlayerbotAIConfig::randomBotAccounts`, and that list is built solely by scanning
account names `<AiPlayerbot.RandomBotAccountPrefix><0..totalAccountCount-1>`
(`RandomPlayerbotFactory.cpp`). A `playerbots_account_type` row of type 1 is enough for
`AssignAccountTypes` to log a character in, but not enough to make it a random bot — and `AiFactory`
adds `grind` and `rpg`/`new rpg` only for random bots, so anything else gets the alt-bot strategy
set and, with no master, stands on its spawn point while its AI counters keep ticking. Name fixture
accounts with the pool prefix and an index below `totalAccountCount` (rows in
`playerbots_account_type` plus any shortfall), and check the log for
`Including non-random bot player <name> into random bot update`, which means the opposite.

A random-bot *name* is not enough either: `AssignAccountTypes` logs in only characters whose
account carries `account_type = 1` in `playerbots_account_type`. It assigns type 1 from the
**lowest** account ids up to `ceil(MaxRandomBots / CalculateAvailableCharsPerAccount())`, and the
AddClass pool (`AiPlayerbot.AddClassAccountPoolSize`, default 50) claims accounts from the
**highest** ids down — so a fixture picked from freshly created characters usually lands on type 2
accounts and nothing logs in at all. The symptom is a cohort that never fills, `Random Bots Stats:
0 online`, repeated `Can't log-in all the requested bots ... N more accounts needed`, and finally an
Observatory `population_timeout` fault with zero bots. Check the startup line `Account type
assignment complete: N RNDbot accounts, M AddClass accounts, K unassigned`, then promote the
fixture's accounts before starting:

```sql
UPDATE playerbots_account_type SET account_type = 1, assignment_date = NOW()
 WHERE account_id IN (SELECT DISTINCT account FROM <characters db>.characters WHERE guid IN (…));
```

Assignment only ever touches type 0 accounts, so a promotion sticks and the module simply tops the
AddClass pool back up from the unassigned remainder. Account types are read at startup, so this is a
pre-start database change, never a live one.

For a run whose whole cohort must be busy, set `AiPlayerbot.BotActiveAlone = 100` with
`AiPlayerbot.botActiveAloneSmartScale = 0`: the stock 10% rotates activity in
`BotActiveAloneDurationSeconds` slices and leaves most of the cohort idle. Record both as deliberate
deviations in the manifest comparison.

Pin a fixture's `randomize`, `teleport` and `level` rows in `playerbots_random_bots` with
`validIn = 0`: `FindEvent` only expires a row when `validIn` is non-zero, so zero pins it for the
whole run regardless of how fast virtual time advances (a far-future `time` happens to work too,
but relies on unsigned wraparound). A pinned bot keeps its prepared level and location — it does
not stop playing, levels normally from the XP it earns, and stays in its starting zone.

Verify movement, not just admission: `activeBots`/`onlineBots` count sessions, so compare per-bot
`x`/`y` across two snapshots and confirm the `activity` mix and `runTotals` advance.

## Disk usage

Run directories (`~/.local/share/azeroth-observatory/runs/<run-id>/`) keep growing `snapshots.ndjson`
files for as long as a run is active, and old runs are not cleaned up automatically. Periodically check
`du -sh ~/.local/share/azeroth-observatory/runs/*/snapshots.ndjson` for stale or abandoned runs (no longer
the active run, not needed for a pending comparison/benchmark) and flag or archive/remove them before disk
space becomes a problem. Never delete a run whose results are still referenced by an open PR, handoff note,
or comparison in progress; ask before removing anything you're unsure is stale.

## Updating dependencies or merging a PR

Changing the Playerbots pin is a separate source change: review core compatibility and the timing/provenance hooks,
retain upstream author history, commit the module change in `jkc-be/mod-playerbots`, push that commit before the core
pin, update `apps/observatory/dependencies.json`, and test the combination. Keep the POV compatibility changes present.

Push core work to a feature branch of the same `jkc-be/azerothcore-wotlk` repository and open a PR against `main`.
Merge only within the user's authorization and after review findings and required checks are resolved. GitHub may not
allow the author to formally approve their own PR; do not impersonate an independent reviewer or bypass required review.
Record source/build checks separately from pending in-game and performance validation in the PR and handoff.

For population-control updates, deploy core, module pin and bridge together. Equal MinRandomBots/MaxRandomBots
provision the pool; Observatory.BotCount is now the initial online target. Preserve an explicit BotGuids allowlist:
it caps the available target and must not be silently widened. Exercise `tests/population_live.py` on an exclusive
disposable run and check pending/matched state, zero bots and pause before declaring the browser control deployed.
