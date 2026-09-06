# Updating the server and Playerbot Observatory

Use `jkc-be/azerothcore-wotlk` `main` as the maintained source. The module revision is the Git submodule pin,
not whatever happens to be latest on module `master`. Read `apps/observatory/docs/HANDOFF.md`, `TIMING.md` and
`VALIDATION.md` before changing the simulation runtime. Preserve the native-client POV integration on ordinary servers.

## Agent update procedure

1. Establish the authorized machine and scope. The default workstation role is client-only. Preparing a PR does not
   authorize deployment. A session instruction explicitly authorizing local/server updates takes precedence. Do not
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
6. Update configuration by reviewing differences from the new `.conf.dist` files; preserve credentials and deployment
   values. Never copy an example over a complete runtime config. For a normal server with simulation disabled, preserve
   databases and use the normal updater. For an observatory run, **never resume a saved virtual-time database**: retain
   the export, restore all four `obs_` databases from the recorded clean pre-run fixture, and select a new run directory.
   Only disposable simulation databases may be reset. Existing production/human data is outside this reset procedure.
7. Start the world with the verified configuration, then start/repoint the bridge to the new spool and retain the
   private token file. Preserve the local extracted artwork directory and the bridge `--maps` argument;
   see `apps/observatory/docs/MAPS.md` for regenerating it after client data changes. Check startup logs, database
   version, module revision and the authoritative `/api/snapshot`.
   Verify a new run ID, advancing simulated time, complete cohort, increasing per-bot AI counters, pause/resume and
   requested versus achieved speed. Reconnect the browser and test an export. If updating ordinary POV, also test its
   watch/switch/stop and successful-action feed with simulation disabled. Do not enable humans in simulation mode.
8. Report the deployed commit IDs, exact services changed, checks performed, dashboard address and any remaining
   correctness/performance limits. If validation fails, keep the new run stopped, retain failure artifacts and restore
   compatible previous binaries/configuration plus the appropriate pre-update DB state. A rollback never reuses virtual
   future timestamps as though a run were resumable. Never claim 100 bots at 10× without benchmark evidence.

## Updating dependencies or merging a PR

Changing the Playerbots pin is a separate source change: review core compatibility and the timing/provenance hooks,
retain upstream author history, commit the module change in `jkc-be/mod-playerbots`, push that commit before the core
pin, update `apps/observatory/dependencies.json`, and test the combination. Keep the POV compatibility changes present.

Push core work to a feature branch of the same `jkc-be/azerothcore-wotlk` repository and open a PR against `main`.
Merge only within the user's authorization and after review findings and required checks are resolved. GitHub may not
allow the author to formally approve their own PR; do not impersonate an independent reviewer or bypass required review.
Record source/build checks separately from pending in-game and performance validation in the PR and handoff.
