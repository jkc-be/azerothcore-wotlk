# Build & tests

Prefer the native host toolchain and host MySQL. Do not use Docker, Podman, Compose, or other
containers to compile or to run the database unless the user explicitly asks for them.

This checkout's conventional install is `env/dist/` (see `var/setup/RUNNING.md`). MySQL is the host
`mysql.service` on port 3306. After a build, start auth+world with `bash var/setup/start-servers.sh`
(imports pending SQL, then the user systemd units). Do not bring up `restart.sh` / `obs-*` containers
for ordinary local runs.

Out-of-source build is required (in-source is blocked).

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/azeroth-server -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSCRIPTS=static -DMODULES=static
make -j$(nproc) && make install
```

C++20 required (`CMAKE_CXX_STANDARD 20`). Useful flags: `BUILD_TESTING=ON` (Google Test), `NOPCH=1` (disable precompiled headers). Full set in `conf/dist/config.cmake`. `compile_commands.json` is exported automatically.

Tests (Google Test, in `src/test/`): configure `-DBUILD_TESTING=ON`, then `ctest` or `./src/test/unit_tests` from the build dir.

## Running an observatory (simulation) world

Read `systems/observatory.md` first; this is only the local startup shape.

`Observatory.Enable = 1` refuses to start unless all four connection strings name `obs_` databases,
so a simulation run never shares the ordinary realm's `acore_*` data. Clone them
(`mysqldump acore_X | mysql obs_X`) — creating the databases needs a privileged step **the user
must run**, because the runtime MySQL account has no `*.*` grants:

```bash
sudo mysql -e "CREATE DATABASE obs_auth; CREATE DATABASE obs_world; CREATE DATABASE obs_characters; \
  CREATE DATABASE obs_playerbots; GRANT ALL PRIVILEGES ON \`obs\_%\`.* TO 'acore'@'localhost';"
```

Render the runtime config (`apps/observatory/render_config.py`), keep it beside the ordinary one as
`env/dist/etc/worldserver-observatory.conf`, and give it its own user unit so `acore-worldserver`
stays untouched. Stop the ordinary realm first — both bind 8085. Then run the bridge as the same
user with `--port` and the run spool.

Two traps specific to this checkout:

- The module config directory is compiled in (`_CONF_DIR/modules`), **not** resolved relative to
  `-c`. Both realms read the same `env/dist/etc/modules/playerbots.conf`, so switch it for the run
  (`obs_playerbots`, `MinRandomBots = MaxRandomBots = <cohort>`, `CommandServerPort = 0`) and keep a
  pristine copy to restore afterwards.
- Every start needs an unused `Observatory.Directory`: worldserver creates it atomically and exits 1
  with `Observatory initialization refused` if it exists. A restart therefore means re-rendering the
  config with a new run directory and repointing the bridge's `--spool`.

Shutting the ordinary realm down can exceed the unit's `TimeoutStopSec` with a large bot population;
if systemd reports `code=killed, signal=KILL`, the world was not saved cleanly. Prefer
`printf 'server shutdown 1\n' > var/run/worldserver.stdin` before stopping the unit.
