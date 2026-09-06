import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('bridge', Path(__file__).parents[1] / 'bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class Controls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.spool = bridge.Spool(self.path, 'test-token')
        self.snapshot = {'run': 'run-a', 'controlSeq': 5, 'fault': ''}
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))

    def test_control_is_atomic_sequenced_and_not_a_false_ack(self):
        result = self.spool.control({'run': 'run-a', 'speed': 10, 'paused': True})
        self.assertEqual(result, {'run': 'run-a', 'sequence': 6, 'status': 'accepted'})
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 6 10 1\n')
        self.assertEqual(self.spool.snapshot()['controlSeq'], 5)

    def test_invalid_and_cross_run_controls_never_reach_world(self):
        for request in [{'run': 'old', 'speed': 1, 'paused': False},
                        {'run': 'run-a', 'speed': True, 'paused': False},
                        {'run': 'run-a', 'speed': 3, 'paused': False},
                        {'run': 'run-a', 'speed': 2, 'paused': 'false'},
                        {'run': 'run-a', 'speed': 2}]:
            with self.assertRaises(ValueError):
                self.spool.control(request)
        self.assertFalse((self.path / 'control.txt').exists())

    def test_faulted_run_cannot_be_resumed(self):
        self.snapshot['fault'] = 'cohort_changed'
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        with self.assertRaises(ValueError):
            self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False})

    def test_viewer_capacity_is_bounded(self):
        for _ in range(16):
            self.assertTrue(self.spool.viewers.acquire(blocking=False))
        self.assertFalse(self.spool.viewers.acquire(blocking=False))


if __name__ == '__main__':
    unittest.main()
