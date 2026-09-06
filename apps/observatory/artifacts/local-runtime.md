# Local runtime checks — 2026-09-06

The user explicitly authorized local server setup. An isolated rootless Podman stack now runs the compiled server,
MySQL 8.4 and the Python dashboard bridge. Four disposable `obs_` databases were provisioned. Client data v20.0 was
extracted, and a clean offline fixture was retained before enabling virtual time. No existing player database was used.

Observed with one identified bot (Nahmis, character GUID 8), without a human game session:

- Authoritative snapshots became ready and the AI counter advanced continuously.
- Live positions changed; combat, casts, damage, NPC death, regeneration, XP and quest acceptance were observed.
- The control proof passed: pause froze bot state while snapshot sequence continued; 1×/2×/5×/10× requests were
  acknowledged. This proves control application, not equal-duration gameplay equivalence.
- The first run's median active sampled speed was approximately 1×. Speed changes retained up to 3.503 simulated
  seconds of debt. Its short benchmark window included catch-up, so it is not an independent 1× throughput result.
- A mixed-clock bot-operation duration produced an invalid slow-operation log. The module now uses real time for
  both the start and elapsed calculation. The incremental build and CTest passed. The server restarted from the
  clean fixture with a new run ID; virtual-time state was not reused.
- The user exercised higher speeds during the second run. Capacity shortfalls were displayed when requested 10×
  exceeded achieved speed. No 100-bot result or sustained 10× claim is made.
- Standard bot randomization generated a higher level and gear. These remain shortcut events, not earned leveling.

Map validation used local `Games/WoW-Reforged` MPQs plus server DBC bounds. It extracted 76 calibrated maps, including
exploration detail textures. A real Chromium session showed the live bot above the Felwood map, selected its marker,
and loaded Kalimdor. All four continent names were available, with no-resident selection covered by a model test. No JavaScript exception
occurred. Seven JavaScript geometry/model tests and four Python control tests passed; the fixture browser smoke also
passed. Screenshots, full journals, configuration, binary hashes and database fixture remain in the private runtime
directory on the machine; client artwork and credentials are not committed.

Pending: controlled baseline/virtual 1×/accelerated comparisons for all required gameplay timing families, ordinary
POV regression coverage, fault injection, and the 100-bot/10× 24-simulated-hour benchmark in `../docs/VALIDATION.md`.

## Population controls — third local run

Core source `426c0fe50b2b523406b1488a1b5231259dc8c435` and module
`c405aac5` were built with clang 18, static scripts/Playerbots and BUILD_TESTING enabled. Worldserver, authserver and
unit_tests built; CTest's unit target passed. The optional Python API was disabled with
`-DMODULE_MOD-PYTHON-API=disabled`, preserving the runtime's previous enabled-module set. Building that separate
handoff alongside Playerbots remains pending its documented external-control patch and C++ compatibility fixes;
its Transport header collides with core and its Boost calls also failed compilation. This is not an all-module build.

The prior virtual-time databases were backed up and all four obs_ databases restored from the clean fixture before
starting run `1788661751568010994`. Initial target: 1; prepared random-bot pool: 100; BotGuids: empty; trace: off.
The private token and extracted map artwork were retained.

- `tests/population_live.py` passed actual 1→3→1→0→3 population changes, resize deferred during pause, active AI
  for all three joined bots, advancing observation/gameplay with zero bots, and monotonic run progression totals.
  It recorded 39 authoritative samples and restored the original controls.
- A real Chromium session then submitted target 3 through the form and observed 3 active/online/in-world bots,
  matched population, pool limit 100 and no JavaScript exceptions. Local map artwork remained visible.
- The character table contained 100 rows both before and after the logout/rejoin proof; shrinking deleted no characters.
- Eight JavaScript tests, seven bridge tests and the synthetic Chromium form smoke passed. Python API's 18 Python
  tests also passed; these do not establish that module's C++ integration works.
- Source review found no remaining issues in the population change. The full C++ linter reports inherited findings
  outside the changed lines; the SQL linter cannot run because this fork has no origin/master. No SQL source changed.

Private evidence: `results/population-live.json`, `results/population-browser.png`, build/test logs, binary hashes,
and the third run's timestamped snapshots/events. This is a small live population proof; equal-duration timing,
100-bot throughput, ordinary-server POV, and overload/fault-injection coverage remain separate pending validations.
