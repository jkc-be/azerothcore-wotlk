# Timing audit and implementation

The pre-edit `timing-inventory.json` records 629 clock call sites in the pinned core, common utilities, scripts and
Playerbots module. `dependencies.json` identifies the exact source revisions. This inventory is source evidence,
not a claim that gameplay timing has passed an end-to-end comparison.

| Timing family | Treatment |
| --- | --- |
| World delta / map updates | Repeated normal 10 ms updates, including joined map workers; no multiplied delta |
| `getMSTime`, `GetTimeMS`, `GetEpochTime` | Common simulation clock while explicitly enabled |
| `GameTime` cached epoch / steady / system points | Same clock, refreshed at each world step |
| Direct steady/system calls in auras, proc cooldowns, transports, world state/events | Routed to the common clock |
| `TaskScheduler` constructors, no-argument updates and group deadlines | Common clock; delta-driven updates unchanged |
| `EventMap`, spells, movement splines, combat swings, regen | Existing delta-based updates at their ordinary step size |
| Bot `time(0/nullptr)`, value cache deadlines, delays and random-bot event expiry | Common simulation epoch |
| Bot scheduled conveniences, randomization, teleports and resurrections | Existing schedules advance in simulated time |
| Bot activity rotation / smart scaling | Explicitly bypassed in isolated runs for every activity, including pathfinding |
| Cohort online/offline expiry | Disabled in isolated runs; periodic rotation prohibited by startup checks |
| Watchdog heartbeat, pacing, networking and socket ping | Real steady clock / Asio timers |
| DB execution durations, deadlock retry budget | Explicit real-time start and elapsed helpers |
| Bot operation execution budget / performance monitor | Real-time measurements remain real |
| Bot RNG shuffle seed | Real entropy retained; not a gameplay deadline or a determinism guarantee |
| Bot account-creation DB waits and sleeps | Real startup/I/O synchronization; clock remains fixed until world ticks |
| External controls, snapshot interval and writer | Real-time scheduling; no pointers cross into I/O threads |

Time acceleration is enabled before world objects and DB state load, establishing one origin. The world thread alone
advances time after the previous update and all map workers finish. Readers see an atomic elapsed counter. Gameplay
steady/epoch clocks do not advance within a tick. No wall clock interposition, compiler macros, damage multipliers,
XP multipliers or movement-speed multipliers are used. Disabled mode follows the pre-existing real-time clocks and
world-update loop. Helpers keep their original types and 32-bit millisecond wrap behavior.

The pacing budget accrues requested simulated microseconds from real elapsed time, then consumes one whole 10 ms step.
Speed changes preserve accumulated debt. Pause accrues no new debt and consumes none; a resume retains outstanding debt.
One tick per outer iteration gives controls and telemetry a chance to run under overload. A single expensive core tick
still delays application of controls until that update ends; it is not safe to interrupt an in-progress map update.
The watchdog can still detect a stuck update. The adapter remains reachable during that stall and displays stale data.

For comparison, `Observatory.RealTimeBaseline = 1` keeps real epoch/steady clocks and ordinary elapsed world deltas
while retaining the identical bots-only isolation, activity and observation setup. It accepts 1× without pause only.
This is a controlled instrumentation baseline, not an assertion that default upstream human-dependent bot activity is
identical to the intentionally forced cohort. Compare that baseline to virtual 1× before comparing 2×/5×/10×.
Baseline mode may overshoot a configured duration by one ordinary update. Virtual mode stops at exact 10 ms boundaries.

The clock is global within this worldserver process. Authserver is separate and keeps real clocks. No human session is
allowed into the accelerated world; console, SOAP, remote admin and the bot command server are disabled to exclude
out-of-band gameplay mutation. Startup requires all four database names to start with `obs_` and a literal disposable
acknowledgement. These checks are guardrails; the operator must actually provision disposable databases and isolated
credentials. Existing production databases must never merely be renamed to satisfy the checks.

Epoch-based saves, bot event deadlines, mail, auctions, respawns, cooldowns and calendar state may persist virtual future
timestamps. A crash does not serialize the simulation clock, queued actions, maps, timers or RNG state. V1 therefore has
no restart/resume. Recreate all four test databases from the same clean fixture for each run. Never reconnect a production
worldserver to these databases. Compare exports across runs; do not use saved character positions as live telemetry.

Known source-level limits requiring server validation: DB completion timing is asynchronous, RNG is not globally seeded,
map ordering and world content are not deterministic, dynamic NPC respawn identity may change, and arbitrary extra modules
have not been audited. Use only the pinned Playerbots module for v1. Profile real tick costs and database queues on the
server; ordinary upstream world diff statistics describe simulated step size in accelerated mode, not achieved throughput.
