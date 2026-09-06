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
