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
1/2/5/10×/Max speed with a backlog target, target-population controls, a **Hold** toggle, and one status line with
alerts and run-state lamps that appear only when a state is not the default.

Pause, speed and population drive the world, so they appear only on a run the world lets the dashboard control. A
read-only feed — an ordinary realm publishing through `mod-alles`, which runs at real time and has no fixed-step clock,
or a recorded run — keeps Hold and says why the rest is absent rather than leaving the deck empty. **Hold** freezes the
view on any feed: history, trails and the journal keep being recorded while nothing redraws, the button reports how
many samples have arrived meanwhile, and releasing shows all of them.

Progression figures cover XP, quests and deaths with
per-simulated-hour rates over a trailing ten-minute window, mean level and mean health.

The **World state** region is the debugging view. *What each bot is working on* lists every planning bot's current
objective — what it is working towards, the approach and step, whatever is obstructing it, how many tries it has had
and how long it has gone without progress — with the planner's own sentence about the decision on hover; obstructed and
stuck bots sort to the top. *Needs attention* collects everything worth checking before calling a run healthy (bots
dead, hurt, silent, not moving, blocked or making no progress; memory stores that are not ready or failed their last
save; a disconnected worker, a spent request budget or a ledger fault; dropped telemetry, refused packets and journal
faults), most serious first and naming the bots each note is about. *Run so far* gives the run's totals beside the rate
they accumulated at, leaving standing figures such as memories held and money carried without a rate. On a planning
world the region also shows the leads bots have passed to each other, how often each paid off, and the questions still
waiting for an answer. A world build that publishes no planning shows the region without the objective and lead
panels. The map colours bots by
activity, health or level, draws the selected bot's trail, a scale bar, a legend with live counts and a hover card.
The roster is a keyboard-navigable list beside the map with an inspector, health and level-XP bars, a per-bot
sparkline, the objective the bot is working on with the planner's own reason for it, and recent events. A timeline with
a readout column, cohort panels (activity share and level band over time, zone table, leaderboards) and a journal
region (live progression feed, record counts, control log with acknowledgement latency, exports, manifest) complete the
page. A jump strip in the bench moves between the regions and marks the one being read. Every figure is derived
from the authoritative snapshots and journal;
rates are computed from retained history and labelled with their window. It uses plain browser JavaScript and a local
Python SSE adapter; neither is coupled to simulation tick frequency. Optional local client map artwork is aligned with
server coordinates; artwork is not bundled in this repository.

History is kept in two tiers. The browser holds up to 4,000 recent samples; the bridge folds every snapshot and
journal record into five-simulated-minute buckets for the whole run (plus a complete milestone snapshot every ten
minutes and every progression record), so the timeline's **Whole run (long term)** window and the
**Journal records per minute** metric cover hours or days without holding the raw journals in memory. With
`Observatory.JournalSegmentBytes` set in the world, the raw journals rotate into segments and the bridge deletes
segments it has folded in beyond `--retain-bytes` (2 GiB per journal by default); the Journal panel reports the
retained and pruned sizes. See [journal retention](docs/INTERFACE.md#journal-retention).

Select **Max** to automatically find a sustainable speed from 1× to 10× in 0.1× steps, such as 2.1× or 3.2×.
This requires an updated worldserver; older servers retain 1×, 2×, 5× and 10×. **Max backlog (ms)** sets
the target, defaulting to 100 simulated milliseconds. The bridge backs off only when debt keeps growing for three seconds and retries
faster settings after five seconds of recovery, even with the browser closed. Brief spikes are possible during adjustment; selecting a numbered
speed ends Max. See [adaptive speed behavior](docs/INTERFACE.md#adaptive-max-speed) for pause, GM POV and restart rules.

The same browser accepts read-only observation feeds from `mod-python-api` controllers via
`acore_api.ObservatoryPublisher`: searchable bots, map/instance selection, health and combat charts, detailed
inspection, recent history restoration and full journal exports. See the Python dashboard guide above.

Runtime configs are rendered, not hand-edited: `render_config.py` layers the installed `.conf.dist`, the tracked
`config/*.conf.example`, a machine-local overlay and a secrets file into the complete `.conf`, and can derive the
overlay from an existing deployment. See `docs/HANDOFF.md`.

Permitted local checks (no server build):

```sh
node --check apps/observatory/web/app.js apps/observatory/web/charts.js apps/observatory/web/model.js
node --test apps/observatory/tests/*.test.mjs
python3 -m unittest discover -s apps/observatory/tests -v
python3 -m py_compile apps/observatory/bridge.py apps/observatory/analyze.py apps/observatory/benchmark.py \
  apps/observatory/render_config.py
python3 apps/codestyle/codestyle-cpp.py
python3 apps/codestyle/codestyle-sql.py
git diff --check
```
