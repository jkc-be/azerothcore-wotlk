import json
import socket
import threading
import unittest
from dataclasses import replace
from unittest.mock import patch

from acore_api import (
    Action, ActionRejected, Client, Events, LiveEnvironment, Observation, OutcomeUnknown, ProtocolError,
    SPELL_CAST_OK, transition,
)


def observation(**overrides):
    data = dict(
        guid="42", registration=1, episode=0, world_tick=10, elapsed_ms=100,
        health=100, max_health=100, power=50, max_power=100, power_type=0, level=1,
        map_id=0, instance_id=0, position=(1.0, 2.0, 3.0, 0.0),
        alive=True, combat=False, casting=False, events=Events(),
    )
    data.update(overrides)
    return Observation(**data)


def response(request_id="1", **overrides):
    result = {
        "version": 1, "id": request_id, "ok": True, "world_tick": "10", "elapsed_ms": "100",
        "spell_result": SPELL_CAST_OK,
        "observation": {
            "guid": "42", "registration": "1", "episode": "0", "health": 100, "max_health": 100,
            "power": 50, "max_power": 100, "power_type": 0, "level": 1, "map": 0, "instance": 0,
            "position": [1, 2, 3, 0], "alive": True, "combat": False, "casting": False,
            "events": {"kills": "0", "deaths": "0", "levels": "0"},
        },
    }
    result.update(overrides)
    return result


class ClientTests(unittest.TestCase):
    def connect(self, handler, timeout=0.5):
        connection, peer = socket.socketpair()
        peer.settimeout(1)
        self.failures = []

        def run():
            try:
                with peer:
                    handler(peer)
            except Exception as error:
                self.failures.append(error)

        worker = threading.Thread(target=run)
        worker.start()
        with patch("acore_api.client.socket.create_connection", return_value=connection):
            client = Client(timeout=timeout)

        def cleanup():
            client.close()
            worker.join(2)
            self.assertFalse(worker.is_alive(), "test peer failed to stop")
            if self.failures:
                raise self.failures[0]

        self.addCleanup(cleanup)
        return client

    def test_fragmented_response_and_monotonic_request_ids(self):
        def peer(connection):
            with connection.makefile("rb") as reader:
                for request_id in ("1", "2"):
                    request = json.loads(reader.readline())
                    self.assertEqual(request["id"], request_id)
                    self.assertEqual(request["bot"], "42")
                    wire = json.dumps(response(request_id)).encode() + b"\n"
                    for start in range(0, len(wire), 7):
                        connection.sendall(wire[start:start + 7])

        client = self.connect(peer)
        for _ in range(2):
            self.assertTrue(client.request("observe", bot="42")["ok"])

    def test_response_for_wrong_action_closes_connection(self):
        def peer(connection):
            connection.recv(4096)
            connection.sendall(json.dumps(response("999")).encode() + b"\n")

        client = self.connect(peer)
        with self.assertRaises(ProtocolError):
            client.request("observe", bot="42")
        with self.assertRaises(ConnectionError):
            client.request("observe", bot="42")

    def test_rejection_preserves_connection(self):
        def peer(connection):
            with connection.makefile("rb") as reader:
                reader.readline()
                denied = response(ok=False, error="claim_required")
                connection.sendall(json.dumps(denied).encode() + b"\n")
                self.assertEqual(json.loads(reader.readline())["id"], "2")
                connection.sendall(json.dumps(response("2")).encode() + b"\n")

        client = self.connect(peer)
        with self.assertRaisesRegex(ActionRejected, "claim_required"):
            client.request("stop", bot="42")
        self.assertTrue(client.request("claim", bot="42")["ok"])

    def test_partial_response_disconnect_is_unknown_and_not_retried(self):
        def peer(connection):
            self.assertEqual(json.loads(connection.recv(4096))["id"], "1")
            connection.sendall(b'{"version":1')

        client = self.connect(peer)
        with self.assertRaises(OutcomeUnknown):
            client.request("reset", bot="42")
        self.assertIsNone(client._socket)

    def test_timeout_discards_connection_without_replay(self):
        def peer(connection):
            connection.recv(4096)
            self.assertEqual(connection.recv(4096), b"")

        client = self.connect(peer, timeout=0.05)
        with self.assertRaises(OutcomeUnknown):
            client.request("cast", bot="42", spell="585", target="43")

    def test_oversized_response_is_bounded(self):
        def peer(connection):
            connection.recv(4096)
            connection.sendall(b"x" * 128)

        client = self.connect(peer)
        client.MAX_RESPONSE = 128
        with self.assertRaisesRegex(ProtocolError, "too large"):
            client.request("list")

    def test_partial_reads_do_not_extend_absolute_deadline(self):
        def peer(connection):
            connection.recv(4096)
            connection.sendall(b"{")
            self.assertEqual(connection.recv(4096), b"")

        client = self.connect(peer, timeout=10)
        with patch("acore_api.client.time.monotonic", side_effect=[0, 1, 11]):
            with self.assertRaises(OutcomeUnknown):
                client.request("list")

    def test_string_boolean_is_rejected(self):
        def peer(connection):
            connection.recv(4096)
            connection.sendall(json.dumps(response(ok="true")).encode() + b"\n")

        client = self.connect(peer)
        with self.assertRaises(ProtocolError):
            client.request("list")


class ModelTests(unittest.TestCase):
    def test_skipped_snapshots_preserve_all_events(self):
        before = observation(events=Events(kills=2, levels=1))
        after = replace(before, world_tick=50, events=Events(kills=5, deaths=1, levels=3))
        self.assertEqual(transition(before, after).events, Events(kills=3, deaths=1, levels=2))
        later = replace(after, world_tick=51)
        self.assertEqual(transition(after, later).events, Events())

    def test_stale_and_replaced_bot_observations_are_rejected(self):
        before = observation()
        for after in (before, replace(before, world_tick=11, registration=2),
                      replace(before, world_tick=11, episode=1)):
            with self.subTest(after=after), self.assertRaises(ValueError):
                transition(before, after)

    def test_counters_cannot_decrease(self):
        with self.assertRaises(ValueError):
            Events(kills=1).since(Events(kills=2))

    def test_invalid_actions_do_not_reach_transport(self):
        for create in (lambda: Action.move_to(float("nan"), 0, 0),
                       lambda: Action.cast(-1, "42"), lambda: Action("reset"),
                       lambda: Action("observe", {"bot": "99"})):
            with self.subTest(create=create), self.assertRaises(ValueError):
                create()

    def test_observation_uses_real_booleans_and_shared_feature_order(self):
        message = response()
        self.assertEqual(Observation.from_response(message).features(), (1, 0.5, 1, 0, 0))
        message["observation"]["combat"] = "false"
        with self.assertRaises(ValueError):
            Observation.from_response(message)

    def test_reset_rebaselines_events_only_after_confirmed_new_episode(self):
        class Peer:
            def request(self, operation, **_):
                result = response()
                if operation == "reset":
                    result["observation"]["episode"] = "1"
                    result["observation"]["events"]["kills"] = "4"
                    result["world_tick"] = "20"
                elif operation == "observe":
                    result["observation"]["episode"] = "1"
                    result["observation"]["events"]["kills"] = "5"
                    result["world_tick"] = "21"
                return result

        environment = LiveEnvironment(Peer(), "42")
        self.assertEqual(environment.reset().episode, 1)
        self.assertEqual(environment.step(Action("observe")).events.kills, 1)

    def test_unconfirmed_reset_is_not_accepted(self):
        class Peer:
            def request(self, *_, **__):
                return response()

        environment = LiveEnvironment(Peer(), "42")
        with self.assertRaisesRegex(ValueError, "did not advance"):
            environment.reset()


if __name__ == "__main__":
    unittest.main()
