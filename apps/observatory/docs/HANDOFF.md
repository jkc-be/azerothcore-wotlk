# Separate-server handoff

For subsequent updates, agents must follow [the update procedure](../../../.agents/docs/systems/observatory.md).

This checkout is prepared on a client-only workstation. Everything below that configures, compiles, provisions,
runs or profiles AzerothCore is for the separate disposable test server. No server access is needed to review the source.

The simulation branch is based on the compatible `mod-playerbots/azerothcore-wotlk` **Playerbot** revision recorded in
`../dependencies.json`. The fork at `jkc-be/mod-playerbots` is a pinned submodule with its own simulation commit.
The upstream histories and AUTHORS/LICENSE files remain intact. Existing main/remotes/local work are preserved.
The carried-over AGENTS instructions continue to apply. No historical SQL file is edited.

On the separate server, with AzerothCore's documented compiler/MySQL/Boost/OpenSSL dependencies already provisioned:

```sh
git clone --branch main --recurse-submodules \
  https://github.com/jkc-be/azerothcore-wotlk.git azerothcore-observatory
cd azerothcore-observatory
git submodule status --recursive
cmake -S . -B ../build-observatory -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=/srv/observatory/server -DSCRIPTS=static -DMODULES=static -DBUILD_TESTING=ON \
  -DMODULE_MOD-PYTHON-API=disabled
cmake --build ../build-observatory --parallel 4
ctest --test-dir ../build-observatory --output-on-failure
cmake --install ../build-observatory
```

Use only the pinned Playerbots module during v1 validation. The separate Python API handoff requires its own
external-control patch; leave it disabled for autonomous observatory runs. Set the parallel build count for the server's memory capacity.
The upstream module requires its Playerbot core; standard AzerothCore is not compatible. See the
[upstream installation guide](https://github.com/mod-playerbots/mod-playerbots#installation) for server prerequisites
and client-data extraction. Supply the licensed 3.3.5a DBC/maps/vmaps/mmaps files to the installed world's `DataDir`.
The UI itself needs no game artwork, node packages or frontend build.

Create a dedicated OS service account and **four new disposable** databases: `obs_auth`, `obs_world`,
`obs_characters`, `obs_playerbots`. Use isolated MySQL credentials restricted to these databases. Initialize them with
the normal core/module updater and clean test fixtures on the server. Keep the auth realm configuration consistent with
this test world. Bots use internal sessions; no human client is required. Native clients are rejected by default;
optional GM-only POV admission is documented in `INTERFACE.md`.
If a provisioning pass is needed before test fixtures exist, do that on the isolated server using ordinary 1× mode;
then take a clean offline fixture before enabling virtual time. Preserve its revision, dump hashes, bot GUIDs,
equipment/levels, known NPC/route/spell setup and effective configuration for every comparison.

Copy installed `worldserver.conf.dist` and `modules/playerbots.conf.dist` into their runtime `.conf` paths. Merge
`../config/worldserver.conf.example` and `../config/playerbots.conf.example`, replacing credentials and paths.
Do not use the fragments as complete configuration files. Do not change rates to achieve acceleration. Begin with one
bot for timing proof by setting BotCount to 1; equal MinRandomBots/MaxRandomBots provision the pool.
`BotGuids`, when supplied, pins exact eligible characters.
Standard upstream convenience settings are retained and recorded. A normal fresh generated population spans upstream
levels 1–80; generation is not earned leveling. Use a prepared low-level fixture to study leveling from a common start.

Create only the parent of the run spool, owned by the worldserver service user:

```sh
install -d -m 700 /srv/observatory/runs
/srv/observatory/server/bin/worldserver -c /srv/observatory/server/etc/worldserver.conf
```

The run directory itself must not exist: worldserver creates it atomically at startup and refuses reuse. The startup
checks also require `DISPOSABLE_BOTS_ONLY`, `obs_` database names, bounded population, no periodic logouts, and disabled
console/RA/SOAP/bot command listeners. Ordinary accounts are rejected in world authentication independently of
bind address.
Keep `Observatory.AllowGmObservers = 0` for comparisons and benchmarks.
The observatory starts at requested 1×. Set speed in the UI or benchmark controller after reviewing initial state.
Signals still stop the process; after normal shutdown the writer drains and closes the journal.

Run the observation adapter as the same OS user (or a deliberately granted local reader/writer) on that server:

```sh
python3 apps/observatory/bridge.py --spool /srv/observatory/runs/run-001 \
  --token-file /srv/observatory/token --port 8787
```

It creates a private token file on first use and binds only to loopback. Use your existing secure tunnel to forward
server port 8787 to the browser machine. Open `http://127.0.0.1:8787`, paste the token into the access field, and connect.
No token is persisted by the browser. Static frontend assets and the Python adapter have no third-party dependencies.
The browser can reconnect freely; it never owns the world loop or queries the character database.

Follow `VALIDATION.md`: baseline clocks → virtual 1× → accelerated small-cohort timing proof → 100-bot 24-simulated-hour
benchmark. Runtime/build failures must be fixed and recorded on the server before claiming the feature validated.
See `INTERFACE.md` for controls, data semantics, backpressure and exports; `TIMING.md` for persistence behavior and audit.

After any run or crash, archive the spool and logs, then recreate all test databases from the clean pre-run fixture.
Do not restart with virtual future deadlines or attempt cross-restart continuation. Use a new run directory every time.

For adjustable population, keep `AiPlayerbot.MinRandomBots = AiPlayerbot.MaxRandomBots = 100` to provision the pool,
then set `Observatory.BotCount` to the desired initial count (for example 1). Clear BotGuids or provide the full
eligible pool; a single pinned GUID intentionally caps growth at one. The dashboard's **Target bots → Set bots** control accepts
0 through the effective pool limit. Zero logs all bots out without deleting characters; Resume applies queued changes.
Update core, the pinned module, and bridge together. See INTERFACE.md for population acknowledgement and timeouts.
