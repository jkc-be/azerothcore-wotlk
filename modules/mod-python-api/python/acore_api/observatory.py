"""Publish controller observations to an Observatory spool without opening a game connection."""

import json
import math
import threading
import time
import uuid
from pathlib import Path

from .model import Action, Observation


class ObservatoryPublisher:
    """One writer per fresh directory. Call publish at the controller's chosen sampling cadence.

    Publishing performs synchronous local file I/O; it never claims, resets or releases bots.
    Observation counters are baselined on first sight and each registration/episode change.
    """

    def __init__(self, directory, *, label="Python controller", publish_interval=0.25):
        if not math.isfinite(publish_interval) or publish_interval < 0:
            raise ValueError("publish_interval must be finite and nonnegative")
        self.directory = Path(directory)
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=False)
        self.run = str(uuid.uuid4())
        self.label = str(label)
        self._start = time.monotonic()
        self._interval = publish_interval
        self._last_publish = float("-inf")
        self._lock = threading.Lock()
        self._bots = {}
        self._previous = {}
        self._totals = dict(kills=0, deaths=0, levelGains=0)
        self._seq = 0
        self._event_seq = 0
        self._closed = False
        self._atomic("manifest.json", {
            "schema": 1, "run": self.run, "source": "python-api", "label": self.label,
            "readOnly": True, "timeBasis": "publisher elapsed real time",
            "population": "last published observations; not a realm online census",
        })
        for name in ("snapshots.ndjson", "events.ndjson"):
            (self.directory / name).touch(mode=0o600)

    def _atomic(self, name, value):
        temporary = self.directory / (name + ".tmp")
        temporary.write_text(json.dumps(value, allow_nan=False) + "\n", encoding="utf-8")
        temporary.replace(self.directory / name)

    def _append(self, name, value):
        with (self.directory / name).open("a", encoding="utf-8") as file:
            file.write(json.dumps(value, allow_nan=False) + "\n")

    def _event(self, guid, kind, detail, elapsed, value=0):
        self._event_seq += 1
        self._append("events.ndjson", {
            "run": self.run, "seq": self._event_seq, "simMs": elapsed,
            "bot": guid, "kind": kind, "detail": detail, "value": value,
        })

    def _snapshot(self):
        elapsed = int((time.monotonic() - self._start) * 1000)
        self._seq += 1
        frame = {
            "schema": 1, "run": self.run, "seq": self._seq, "source": "python-api",
            "label": self.label, "readOnly": True, "simMs": elapsed, "realMs": elapsed,
            "publishedUnixMs": int(time.time() * 1000), "completed": self._closed,
            "runTotals": dict(self._totals), "bots": list(self._bots.values()),
        }
        self._append("snapshots.ndjson", frame)
        if self._seq == 1:
            self._atomic("initial.json", frame)
        self._atomic("latest.json", frame)
        self._last_publish = time.monotonic()

    def publish(self, observation: Observation, *, name=None, action: Action | None = None, spell_result=None):
        """Record a sampled observation and optionally its acknowledged action (not a guaranteed effect).

        Names come from Client.list_bots(); GUIDs remain strings. Unpublished bots retain their
        last observation and timestamp until remove() is called. At most 64 bots may be retained.
        """
        with self._lock:
            if self._closed:
                raise RuntimeError("publisher is closed")
            guid = observation.guid
            previous = self._previous.get(guid)
            if guid not in self._bots and len(self._bots) >= 64:
                raise ValueError("publisher supports at most 64 bots")
            if previous and (observation.registration, observation.episode, observation.world_tick) <= (
                    previous.registration, previous.episode, previous.world_tick):
                raise ValueError("stale observation")
            same_episode = previous and (previous.registration, previous.episode) == (
                observation.registration, observation.episode)
            delta = observation.events.since(previous.events) if same_episode else None
            elapsed = int((time.monotonic() - self._start) * 1000)
            x, y, z, orientation = observation.position
            bot = {
                "id": guid, "name": str(name or self._bots.get(guid, {}).get("name", guid)),
                "map": observation.map_id, "instance": observation.instance_id, "zone": None,
                "x": x, "y": y, "z": z, "orientation": orientation, "level": observation.level,
                "health": observation.health, "maxHealth": observation.max_health,
                "power": observation.power, "maxPower": observation.max_power, "powerType": observation.power_type,
                "alive": observation.alive, "combat": observation.combat, "casting": observation.casting,
                "activity": "dead" if not observation.alive else "casting" if observation.casting
                else "combat" if observation.combat else "idle",
                "worldTick": observation.world_tick, "registration": observation.registration,
                "episode": observation.episode, "apiElapsedMs": observation.elapsed_ms,
                "observedMs": elapsed, "observedUnixMs": int(time.time() * 1000),
                "kills": observation.events.kills, "deaths": observation.events.deaths,
                "levelGains": observation.events.levels,
            }
            json.dumps(bot, allow_nan=False)
            if not same_episode:
                self._event(guid, "baseline", "Observation baseline established", elapsed)
            if delta:
                for key, total in (("kills", "kills"), ("deaths", "deaths"), ("levels", "levelGains")):
                    value = getattr(delta, key)
                    self._totals[total] += value
                    if value:
                        self._event(guid, key, "", elapsed, value)
            if action:
                detail = action.operation
                if action.arguments:
                    detail += " " + json.dumps(dict(action.arguments), sort_keys=True)
                if spell_result is not None:
                    detail += f" (spell result {spell_result})"
                self._event(guid, "action_acknowledged", detail, elapsed)
            self._previous[guid] = observation
            self._bots[guid] = bot
            if time.monotonic() - self._last_publish >= self._interval:
                self._snapshot()

    def flush(self):
        """Publish all latest samples immediately, e.g. before the controller waits for input."""
        with self._lock:
            if self._closed:
                raise RuntimeError("publisher is closed")
            self._snapshot()

    def remove(self, guid):
        """Remove a bot from the displayed cohort after release/logout; preserve run totals."""
        with self._lock:
            if self._closed:
                raise RuntimeError("publisher is closed")
            if str(guid) in self._bots:
                del self._bots[str(guid)]
                del self._previous[str(guid)]
                elapsed = int((time.monotonic() - self._start) * 1000)
                self._event(str(guid), "removed", "Removed from observation feed", elapsed)
                self._snapshot()

    def close(self):
        with self._lock:
            if not self._closed:
                self._closed = True
                self._snapshot()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
