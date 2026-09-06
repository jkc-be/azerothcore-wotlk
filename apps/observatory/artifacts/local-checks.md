# Validation record

Checked source: core `7a91338ac2c02c591838b74bb27849487c4ceca2`, Playerbots `5f5237dd3df7dc5c8099733a8e2661467c24f467`.
Subsequent documentation-only commits do not change the tested source.

- Full authorized local build passed: Ubuntu 24.04 rootless Podman container, clang 18, CMake/Ninja,
  RelWithDebInfo, static scripts and modules, `BUILD_TESTING=ON`, `TOOLS_BUILD=none`, parallelism 12.
  Built worldserver, authserver, the pinned module and unit_tests; no server executable was launched or installed.
- CTest passed. Google Test: 5,917 passed, 5,486 parameter cases skipped, one disabled; no failures.
  The three SimulationBudget cases and SimulationClockDeathTest all passed. The latter verifies paused/advancing
  gameplay clocks, GameTime, TaskScheduler and EventMap while real time continues in a subprocess.
- Browser model: four Node tests passed. Control mailbox: four Python tests passed.
- Chromium smoke passed: actual browser rendered fixture SSE, selected a bot, acknowledged pause and speed requests,
  with no JavaScript exceptions. The fixture and screenshot are synthetic, not measured gameplay.
- JavaScript/Python syntax and core/module whitespace checks passed.
- Core and module C++ codestyle report inherited findings; none intersect the changed lines relative to main and the
  recorded module upstream. Full-tree lint is not a pass. SQL codestyle cannot fetch absent `origin/master`;
  this feature changes no SQL relative to main. Fork GitHub CI jobs skip and do not supply build evidence.
- No game server was launched. Small-cohort gameplay equivalence and 100-bot/10× performance remain unmeasured.
  Follow `../docs/VALIDATION.md`, including ordinary-server/POV regression checks with simulation disabled.

Reproduce source/frontend checks using the README. Build and CTest commands are in `../docs/HANDOFF.md`.
The local build used an out-of-source directory with `-G Ninja -DCMAKE_CXX_COMPILER=clang++
-DCMAKE_C_COMPILER=clang -DTOOLS_BUILD=none` in addition to those documented options.
