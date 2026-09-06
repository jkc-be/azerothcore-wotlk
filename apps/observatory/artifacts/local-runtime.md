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
