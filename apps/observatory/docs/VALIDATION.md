# Validation protocol (separate game server)

Status at handoff: source/front-end checks have run locally. No C++ compilation, worldserver execution, gameplay timing
comparison or throughput measurement has run on the client workstation. The 10×/100-bot target is unmeasured.
Do not scale to 100 until the small timing proof passes. Keep the resulting exports with the revision and configuration.

1. Follow `HANDOFF.md` to build on the separate server. Run
   `ctest --test-dir ../build-observatory --output-on-failure` and the dedicated
   `../build-observatory/src/test/unit_tests --gtest_filter=SimulationBudget.*` tests.
   They exercise fixed step count, fractional debt, overload, pause and speed transitions without launching a realm.
2. Provision one to five known bots in a clean disposable fixture. Set `Observatory.BotCount` and both
   `AiPlayerbot.MinRandomBots` / `MaxRandomBots` to the same count. Set `Observatory.BotGuids` to their exact character
   GUID counters (comma-separated, no spaces) to make selection repeatable. They must be eligible characters on random
   bot accounts in the fixture. The optional list is validated for count and uniqueness; unavailable identities leave
   the run visibly in startup. If omitted, the first complete automatically selected cohort is identified and locked.
3. Enable `Observatory.Trace = 1`, choose a fresh run directory, and start with 1×. Let bots finish initialization.
   Preserve manifest, initial snapshot, full events and snapshots. Confirm that every cohort member's AI counter grows
   without a human session, positions move, actual quest state changes and at least one combat/cast cycle appears.
   Check events for factory gear, random level generation, resource conveniences, teleport attempts and resets.
4. Test controls using `benchmark.py --control-proof --bots 1 --hours 0.05 --speed 1` (use the actual cohort count).
   This verifies fresh observation during two real seconds of frozen gameplay and acknowledges every speed.
   Also pause while a bot is casting, moving and dead; resume and inspect the trace. Paused snapshots must have identical
   sim time and bot state. Scheduled casts, auras, respawns and bot conveniences must resume from their remaining times.
5. Establish equal-duration controlled comparisons, not a comparison of unrelated random fights. On a clean fixture,
   arrange a fixed-level/equipment bot, a known NPC and an unobstructed route. Record the DB/DBC IDs and spell data used.
   Use server-side fixture preparation before enabling isolation; console/human commands are deliberately refused during
   a run. Save the prepared fixture before simulation starts and clone it independently for every run. Restore all four
   DBs, use the same GUID selection and settings, and choose a new spool. Do not reuse virtual-time output DBs as fixtures.
6. First run `Observatory.RealTimeBaseline = 1` (real clocks, ordinary elapsed deltas) with the same forced activity and
   telemetry. Then run virtual 1× with `Observatory.RealTimeBaseline = 0`. Compare those before testing 2×, 5× and 10×.
   Use `Observatory.DurationMs = 600000` for a ten-minute window after cohort readiness, or a longer fixed window needed
   for the selected creature's actual corpse/respawn delay. Baseline may overshoot by one update; compare the common window.
   For accelerated runs, request the speed before the comparison window starts and record that acknowledgement.
7. Run `python3 apps/observatory/analyze.py /srv/observatory/runs/one --compare /srv/observatory/runs/ten
   --start-ms START --duration-ms 600000 > timing-comparison.json` (on one shell line). Select the same controlled phase
   in both runs; if phase offsets differ, call the Python `trace()` function with separate start offsets, or export each
   chosen window. Inspect the underlying trace as well as aggregates. Missing required events mean the scenario did not
   exercise that behavior, not a pass.

| Required behavior | Evidence and acceptance |
| --- | --- |
| Damage | Same spell/weapon/level/gear and modifiers; per-hit values/distribution do not scale with requested speed |
| Attacks and periodic effects | `melee_swing` / `aura_tick` counts and intervals for the same alive/engaged simulated window |
| Casts | Match cast start/finish/cancel by bot and spell; ordinary cast time with at most one step quantization difference |
| Cooldowns | Same duration records; next cast eligibility respects simulated elapsed cooldown, including pause |
| Movement | Per-step world coordinates and path length on the same route; exclude documented teleport attempts/transfers |
| Regeneration | Health/power changes and regeneration intervals with same combat, aura, food and convenience state |
| Deaths | Same controlled lethal input; `death` / creature state records, corpse and ghost behavior |
| Respawns | NPC death and respawn timing including configured corpse delay; inspect changed dynamic GUIDs separately |
| Bot actions | Same schedule deadlines for randomization, refresh, travel and other conveniences; compare intervals |
| Baseline virtual 1× | All above agree with the real-clock instrumentation baseline within tick/RNG variation |

Do not assert global determinism: asynchronous DB completion, RNG, encounters and map order may diverge. For random
combat repeat scenarios and compare distributions; for fixed cooldowns and cast times compare exact spell parameters
and event intervals. Record tolerances before running. `analyze.py` deliberately does not issue a gameplay PASS verdict.

Additional integration checks:

- Attempt a WoW-client world login: world authentication must close the connection even with valid credentials.
  Authserver login alone is not the test. Verify that neither console, SOAP, RA nor bot command server can mutate gameplay.
- Disconnect every viewer for several minutes. Reconnect and verify AI counters and simulated time continued; export
  the retained journal. Open a slow SSE viewer and verify its timeout affects only that connection. Test more than 16
  viewers: excess viewers receive 503; the world continues. Kill/restart the bridge with the same spool/token and verify
  sequence-safe controls and immediate current state. Tokens never belong in query strings or export files.
- Under a server CPU quota, request 10× and check achieved rate below requested rate, increasing backlog, normal 10 ms
  steps, and continued AI updates for all bots. Remove the quota; debt must remain and drain without gameplay step loss.
  Record actual cgroup/quota commands used; never run this experiment against a shared production server.
- Remove disk write capacity on a disposable test spool. The run must freeze or become stale with an explicit writer
  fault when detectable. This run is invalid even if some events were exported; start again with fresh DBs and spool.
- Test an incorrect acknowledgement, an existing spool path, a non-`obs_` database, mismatched bot counts and periodic
  bot logouts: initialization must refuse. Test simulation disabled on the normal server baseline separately.

Only after correctness passes, use 100 bots with trace disabled, `DurationMs = 0`, and run:

```sh
python3 apps/observatory/benchmark.py --token-file /srv/observatory/token \
  --output /srv/observatory/results/100bots-10x --bots 100 --speed 10 --hours 24
python3 apps/observatory/analyze.py /srv/observatory/runs/run-100 > /srv/observatory/results/summary.json
```

The benchmark writes hardware, sampled authoritative telemetry and measured results, records shortfall samples,
checks the cohort and AI freshness, and pauses on successful completion. A lower achieved rate is a capacity shortfall,
not permission to skip steps. The default 30-real-hour deadline bounds a failed/slow run. Preserve world logs, module
performance logs and server CPU/DB profiling beside the report. Profile world/map updates, navigation, AI and database
queues with the server's normal profiler; do not infer a bottleneck from fixed simulated world-diff statistics.
A throughput result alone does not establish correctness. Fill `artifacts/server-results.template.json` only with
measured data and link each correctness claim to its scenario and journal window.
