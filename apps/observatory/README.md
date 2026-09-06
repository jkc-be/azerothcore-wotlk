# Playerbot Observatory

Source implementation of a browser observatory and opt-in fixed-step time acceleration for an isolated Playerbot
AzerothCore world. The target is 100 continuously active bots at 10×; achieved performance and gameplay equivalence
must be measured on the separate server. Build checks were explicitly authorized locally; no game server was launched.

- [Agent update procedure](../../.agents/docs/systems/observatory.md)
- [Server integration and startup](docs/HANDOFF.md)
- [Timing audit and persistence](docs/TIMING.md)
- [Telemetry and control interface](docs/INTERFACE.md)
- [Timing proof and benchmark protocol](docs/VALIDATION.md)
- [Pinned upstream revisions](dependencies.json)
- [Validation record](artifacts/local-checks.md)

The dashboard plots authoritative world coordinates, inspects bots and quests, charts progression against simulated
time, controls pause and requested speed, and exports timestamped events. It uses plain browser JavaScript and a local
Python SSE adapter; neither is coupled to simulation tick frequency. The coordinate map contains no proprietary tiles.

Permitted local checks (no server build):

```sh
node --check apps/observatory/web/app.js
node --test apps/observatory/tests/model.test.mjs
python3 -m unittest discover -s apps/observatory/tests -v
python3 -m py_compile apps/observatory/bridge.py apps/observatory/analyze.py apps/observatory/benchmark.py
python3 apps/codestyle/codestyle-cpp.py
python3 apps/codestyle/codestyle-sql.py
git diff --check
```
