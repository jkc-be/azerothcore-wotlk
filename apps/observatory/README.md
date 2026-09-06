# Playerbot Observatory

Source implementation of a browser observatory and opt-in fixed-step time acceleration for an isolated Playerbot
AzerothCore world. The target is 100 continuously active bots at 10×; achieved performance and gameplay equivalence
must be measured on the separate server. A local one-bot run has been exercised; full gameplay equivalence and
population benchmarks remain pending.

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

The dashboard plots authoritative world coordinates, inspects bots and quests, charts progression against simulated
time, controls target population, pause and requested speed, and exports timestamped events. It uses plain browser
JavaScript and a local Python SSE adapter; neither is coupled to simulation tick frequency. Optional local client map artwork is aligned
with server coordinates; artwork is not bundled in this repository.

The same browser accepts read-only observation feeds from `mod-python-api` controllers via
`acore_api.ObservatoryPublisher`: searchable bots, map/instance selection, health and combat charts, detailed
inspection, recent history restoration and full journal exports. See the Python dashboard guide above.

Permitted local checks (no server build):

```sh
node --check apps/observatory/web/app.js
node --test apps/observatory/tests/*.test.mjs
python3 -m unittest discover -s apps/observatory/tests -v
python3 -m py_compile apps/observatory/bridge.py apps/observatory/analyze.py apps/observatory/benchmark.py
python3 apps/codestyle/codestyle-cpp.py
python3 apps/codestyle/codestyle-sql.py
git diff --check
```
