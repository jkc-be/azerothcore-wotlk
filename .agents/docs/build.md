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

## Dashboard map artwork

Until the bridge is given extracted client artwork the dashboard draws bot positions on bare
coordinates — no error, the terrain is simply absent. Installing a client is not enough: the tiles
must be extracted once, and the bridge must be started with `--maps`. See
`apps/observatory/docs/MAPS.md` for what the extractor produces.

```bash
python3 -m venv "$TOOLS"
"$TOOLS/bin/pip" install -r apps/observatory/requirements-map-extractor.txt
"$TOOLS/bin/python" apps/observatory/extract_maps.py \
  --client /path/to/World-of-Warcraft \
  --dbc env/dist/data/dbc/WorldMapArea.dbc \
  --output "$ART"
```

`WorldMapOverlay.dbc` must sit beside `WorldMapArea.dbc`; this checkout already has both under
`env/dist/data/dbc/`. A complete 3.3.5a client yields 76 calibrated maps (~2400 PNGs, ~190 MB);
fewer means incomplete base maps were skipped and those zones keep coordinate rendering. Keep the
output outside Git, always pass a fresh `--output` (the extractor refuses an existing directory),
and re-extract after client data changes. A client carrying custom patch archives yields that custom
art, because patches take precedence over base assets.

Then add `--maps <art dir>` to the bridge's user unit, `systemctl --user daemon-reload`, and restart
it. Verify through the API rather than the browser: `/api/maps` must list the areas and
`/api/maps/<area>-1.png` must return `image/png`. Both need the token as an `Authorization: Bearer`
header — the `?token=` query form is rejected.

Restarting the bridge drops speed control to manual, so a run that was on Max keeps its last speed
and stops adapting. Re-select it afterwards with
`POST /api/control {"run": …, "speed": "max", "paused": false, "backlogLimitMs": …}`; Max re-arms
from 1× and ramps back up, which restarts its backlog measurement.

If the host has no `pip` and no `python3-venv`, `python3 -m venv` fails with `ensurepip is not
available`. Bootstrap without touching the system and without `sudo apt`: create the venv with
`--without-pip`, then run `https://bootstrap.pypa.io/get-pip.py` with that venv's interpreter.
Never install the extractor's dependencies into an unrelated project's virtualenv.
