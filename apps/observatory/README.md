# Playerbot Observatory

Source implementation of a browser observatory and opt-in fixed-step time acceleration for an isolated Playerbot
AzerothCore world. The target is 100 continuously active bots at 10×; achieved performance and gameplay equivalence
must still be measured on a disposable observatory world. A local one-bot run has been exercised; full gameplay
equivalence and population benchmarks remain pending.

- [Agent update procedure](../../.agents/docs/systems/observatory.md)
- [Server integration and startup](docs/HANDOFF.md)
- [Timing audit and persistence](docs/TIMING.md)
- [Telemetry and control interface](docs/INTERFACE.md)
- [Timing proof and benchmark protocol](docs/VALIDATION.md)
- [Pinned upstream revisions](dependencies.json)
- [Build validation record](artifacts/local-checks.md)
- [Local runtime and map checks](artifacts/local-runtime.md)
- [Local map artwork setup](docs/MAPS.md)
- [Python controller dashboard and synthetic preview](docs/PYTHON.md)

The dashboard is a simulation control panel. A sticky instrument bench carries the simulated-time clock, a linear
achieved-versus-requested speed instrument, backlog and longest-tick figures with sparklines, pause/resume,
1/2/5/10×/Max speed with a backlog target, target-population controls, and one status line with alerts and run-state
lamps that appear only when a state is not the default. Progression figures cover XP, quests and deaths with
per-simulated-hour rates over a trailing ten-minute window, mean level and mean health. The map colours bots by
activity, health or level, draws the selected bot's trail, a scale bar, a legend with live counts and a hover card.
The roster is a keyboard-navigable list beside the map with an inspector, health and level-XP bars, a per-bot
sparkline and recent events. A timeline with a readout column, cohort panels (activity share and level band over time,
zone table, leaderboards) and a journal region (live progression feed, record counts, control log with acknowledgement
latency, exports, manifest) complete the page. Every figure is derived from the authoritative snapshots and journal;
rates are computed from retained history and labelled with their window. It uses plain browser JavaScript and a local
Python SSE adapter; neither is coupled to simulation tick frequency. Optional local client map artwork is aligned with
server coordinates; artwork is not bundled in this repository.

Select **Max** to automatically find the fastest sustainable preset (1×, 2×, 5× or 10×). **Max backlog (ms)** sets
the target, defaulting to 100 simulated milliseconds. The bridge backs off as debt grows and periodically retries
faster settings, even with the browser closed. Brief spikes are possible during adjustment; selecting a numbered
speed ends Max. See [adaptive speed behavior](docs/INTERFACE.md#adaptive-max-speed) for pause, GM POV and restart rules.

The same browser accepts read-only observation feeds from `mod-python-api` controllers via
`acore_api.ObservatoryPublisher`: searchable bots, map/instance selection, health and combat charts, detailed
inspection, recent history restoration and full journal exports. See the Python dashboard guide above.

Permitted local checks (no server build):

```sh
node --check apps/observatory/web/app.js apps/observatory/web/charts.js
node --test apps/observatory/tests/*.test.mjs
python3 -m unittest discover -s apps/observatory/tests -v
python3 -m py_compile apps/observatory/bridge.py apps/observatory/analyze.py apps/observatory/benchmark.py
python3 apps/codestyle/codestyle-cpp.py
python3 apps/codestyle/codestyle-sql.py
git diff --check
```
