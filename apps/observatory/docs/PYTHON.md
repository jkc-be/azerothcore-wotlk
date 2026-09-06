# Python observation dashboard

The browser can display observations published by an existing Python controller. This path works independently of
the accelerated Observatory simulation. It opens no additional game connection and does not claim or reset bots.
The Python package, bridge and browser require no additional dependencies or server rebuild for this integration.
Live observations still require an already installed, enabled `mod-python-api` and its existing controller.

## Connect an existing controller

Use `ObservatoryPublisher` from `modules/mod-python-api/python/acore_api`. Create a fresh run directory by constructing
the publisher; do not reuse an existing simulation spool. Add publication to the controller that already owns the bots:

```python
from acore_api import ObservatoryPublisher

# env is your existing LiveEnvironment, bot_name comes from Client.list_bots().
# Keep the existing controller's action selection and reset/release lifecycle.
with ObservatoryPublisher("/tmp/python-observations-001") as dashboard:
    dashboard.publish(env.observation, name=bot_name)
    for action in actions:  # Your existing action loop.
        result = env.step(action, wait_ticks=4)
        dashboard.publish(
            result.observation, name=bot_name,
            action=action, spell_result=result.spell_result,
        )
```

Use one publisher for all bots controlled by the process. Publishing performs synchronous local file I/O. Full
snapshots are coalesced to at most four per second across the feed by default (`publish_interval=0.25`); explicit
`flush()`, removal and close also publish immediately. Call `flush()` before waiting for input to expose pending
samples. Events are journaled on each call; choose a sampling cadence appropriate to the controller. This does
not introduce extra `observe` requests. When sampling, only pass `action` for the corresponding acknowledged action;
intermediate actions are not reconstructed. Publish `env.observation` after a successful reset to establish the new
episode baseline. Call `dashboard.remove(guid)` when releasing a bot or removing it from the observation cohort.

Start the bridge on the machine holding that directory:

```sh
python3 apps/observatory/bridge.py --spool /tmp/python-observations-001 \
  --token-file /tmp/python-observatory.token --port 8788
```

Open `http://127.0.0.1:8788`, paste the contents of the token file and select **Connect**. For a remote controller, use
your existing authenticated port-forward to the bridge's loopback port. The browser and publisher may start in either
order; the browser waits for the first snapshot. Optional `--maps /path/to/artwork` uses the existing
[local client artwork](MAPS.md). Without artwork, the map still plots world coordinates.

The bot list searches names and GUIDs across all maps. Selecting a bot centers the map and selects its instance.
Python observations do not include zone IDs; their markers remain available when selecting zone artwork on the same
map. The inspector shows coordinates, health, power, activity, world tick, registration, episode, counters and age.
Unknown XP and quest data are not shown as zero. Charts include observed population, alive/combat counts, mean health,
level distribution and accumulated kill/death/level deltas.

## Meaning of the data

- **Observed population is not online population.** Each marker is the last published sample for that bot. Observations
  across bots are sequential, not a synchronized world snapshot. Unpublished bots remain visible until removed.
- Samples older than three seconds are marked stale and dimmed. Age uses the publisher's wall timestamp; synchronize
  browser and publisher clocks. A clean publisher close marks the feed closed; a crash is detected through staleness.
- Charts use elapsed real time since publisher creation. `simMs` is retained as the browser's shared timestamp field,
  but the manifest and UI identify this time basis. World ticks and API elapsed world time remain in each sample.
  API elapsed time sums world-update diffs since the API bridge started; it does not restart with an episode.
- Run totals accumulate counter differences between published samples in the same registration and episode. First
  sight, re-registration and reset establish new baselines. Totals survive removal. Changes across a reset or an
  unobserved/released interval cannot be reconstructed.
  Raw counters in the inspector start at registration and continue through resets. Create a new publisher run after
  reconnecting the game controller; server registration identities and counters are process-local.
- `action_acknowledged` describes the reported action request and optional spell result, not guaranteed arrival,
  damage, successful spell completion or a full combat log. Only explicitly published actions enter the journal.
- Pause, speed and population controls are disabled, and the bridge rejects control requests for Python feeds.
  Viewing or disconnecting the browser does not affect bot ownership or AI.

On connect/reconnect, the bridge returns up to 1,000 recent complete snapshots from a bounded 4 MiB journal tail.
The browser retains up to 4,000 chart samples during a session. Full `snapshots.ndjson` and `events.ndjson`, plus
`manifest.json` and `initial.json`, remain downloadable. These files are observation history, not resumable game state.
The simulation timing analyzer/benchmark expects simulation telemetry; use the UI or exported records directly for
Python feeds. Closing the browser does not stop disk recording; manage run storage and publisher lifetime
in the controller.

## Preview without a game server

This generates explicitly labelled synthetic data with eight moving markers. It never contacts MySQL or worldserver.
Use a fresh directory name each time:

```sh
python3 apps/observatory/python_demo.py --spool /tmp/observatory-demo-001
```

In another terminal:

```sh
python3 apps/observatory/bridge.py --spool /tmp/observatory-demo-001 \
  --token-file /tmp/observatory-demo.token --port 8788
```

Open the browser as above. The demo stops after five minutes, retaining its history; override with `--seconds N`.
Ctrl+C also stops the publisher cleanly. The bridge keeps serving the recorded history until stopped.

## Validation

```sh
PYTHONPATH=modules/mod-python-api/python python3 -m unittest discover \
  -s modules/mod-python-api/python/tests -v
python3 -m unittest discover -s apps/observatory/tests -v
node --test apps/observatory/tests/*.test.mjs
python3 apps/observatory/tests/browser_python.py
python3 apps/observatory/tests/browser_smoke.py
```

Browser checks require the existing local Google Chrome and Node 22 tooling. The Python check exercises the real
publisher, HTTP/history/SSE and browser with synthetic observations, including search, inspection, chart selection,
reload, exports and rejected controls. It does not validate Python-controlled gameplay on a running realm.
