import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

from acore_api import Action, Events, ObservatoryPublisher
from test_api import observation


class PublisherTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "run"
        self.publisher = ObservatoryPublisher(self.path, publish_interval=0)

    def read(self, name="latest.json"):
        return json.loads((self.path / name).read_text())

    def test_publish_never_connects_and_preserves_identity_and_missing_fields(self):
        with patch("socket.create_connection", side_effect=AssertionError("must not connect")):
            self.publisher.publish(observation(guid="18446744073709551614"), name="A bot")
        frame = self.read()
        self.assertTrue(frame["readOnly"])
        bot = frame["bots"][0]
        self.assertEqual(bot["id"], "18446744073709551614")
        self.assertEqual(bot["name"], "A bot")
        self.assertIsNone(bot["zone"])
        self.assertNotIn("xp", bot)
        self.assertNotIn("onlineBots", frame)
        self.assertEqual(self.read("initial.json"), frame)

    def test_episode_baselines_departures_and_stale_observations(self):
        first = observation(events=Events(kills=10, deaths=2))
        self.publisher.publish(first)
        current = replace(first, world_tick=12, events=Events(kills=12, deaths=3))
        self.publisher.publish(current, action=Action("observe"))
        with self.assertRaisesRegex(ValueError, "stale"):
            self.publisher.publish(first)
        with self.assertRaisesRegex(ValueError, "backwards"):
            self.publisher.publish(replace(current, world_tick=13, events=Events()))
        self.publisher.publish(replace(current, world_tick=14, episode=1, events=Events()))
        self.assertEqual(self.read()["runTotals"], dict(kills=2, deaths=1, levelGains=0))
        self.publisher.remove(first.guid)
        self.assertEqual(self.read()["bots"], [])
        self.assertEqual(self.read()["runTotals"]["kills"], 2)
        journal = [json.loads(line) for line in (self.path / "events.ndjson").read_text().splitlines()]
        self.assertEqual([entry["kind"] for entry in journal],
                         ["baseline", "kills", "deaths", "action_acknowledged", "baseline", "removed"])

    def test_sequential_bots_keep_independent_sample_age_and_close_is_final(self):
        with patch("acore_api.observatory.time.time", return_value=100):
            self.publisher.publish(observation(), name="First")
        with patch("acore_api.observatory.time.time", return_value=105):
            self.publisher.publish(observation(guid="43"), name="Second")
        first, second = self.read()["bots"]
        self.assertEqual(second["observedUnixMs"] - first["observedUnixMs"], 5000)
        self.publisher.close()
        self.assertTrue(self.read()["completed"])
        self.assertEqual(len(self.read()["bots"]), 2)
        with self.assertRaises(RuntimeError):
            self.publisher.publish(observation(world_tick=20))
        with self.assertRaises(FileExistsError):
            ObservatoryPublisher(self.path)
        frames = [json.loads(line) for line in (self.path / "snapshots.ndjson").read_text().splitlines()]
        self.assertEqual([frame["seq"] for frame in frames], [1, 2, 3])

    def test_snapshot_rate_is_per_feed_and_flush_keeps_the_latest_samples(self):
        publisher = ObservatoryPublisher(self.path.parent / "bounded", publish_interval=60)
        for index in range(20):
            publisher.publish(observation(guid=str(index)))
        path = publisher.directory / "snapshots.ndjson"
        self.assertEqual(len(path.read_text().splitlines()), 1)
        publisher.flush()
        frames = [json.loads(line) for line in path.read_text().splitlines()]
        self.assertEqual(len(frames), 2)
        self.assertEqual(len(frames[-1]["bots"]), 20)
        publisher.close()


if __name__ == "__main__":
    unittest.main()
