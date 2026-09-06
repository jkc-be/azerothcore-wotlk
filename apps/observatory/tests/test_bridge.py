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

    def test_population_coalesces_without_losing_target_on_speed_change(self):
        self.snapshot.update(expectedBots=1, maxBots=100)
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        self.spool.control({'run': 'run-a', 'speed': 1, 'paused': True, 'bots': 3})
        restarted = bridge.Spool(self.path, 'test-token')
        result = restarted.control({'run': 'run-a', 'speed': 5, 'paused': False})
        self.assertEqual(result['sequence'], 7)
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 7 5 0 3\n')
        restarted.control({'run': 'run-a', 'speed': 5, 'paused': False, 'bots': 0})
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 8 5 0 0\n')

    def test_population_validation_and_pool_limit(self):
        self.snapshot.update(expectedBots=1, maxBots=3)
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        for bots in (-1, 4, 101, True, 1.5, '2', None):
            with self.assertRaises(ValueError):
                self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'bots': bots})
        self.assertFalse((self.path / 'control.txt').exists())
        self.snapshot['baseline'] = True
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        with self.assertRaises(ValueError):
            self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'bots': 2})

    def test_old_world_rejects_population_control(self):
        with self.assertRaises(ValueError):
            self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'bots': 2})

    def test_observer_lock_preserves_population_control(self):
        self.snapshot.update(observers=1, expectedBots=1, maxBots=100)
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        for speed, paused in ((2, False), (5, False), (10, False), (1, True)):
            with self.assertRaisesRegex(ValueError, 'GM POV'):
                self.spool.control({'run': 'run-a', 'speed': speed, 'paused': paused})
        self.assertFalse((self.path / 'control.txt').exists())
        self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'bots': 3})
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 6 1 0 3\n')
        self.snapshot['observers'] = 0
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        self.spool.control({'run': 'run-a', 'speed': 10, 'paused': False})
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 7 10 0 3\n')

    def test_rejected_mailbox_target_is_not_reapplied(self):
        self.snapshot.update(expectedBots=1, maxBots=100, controlError='GM POV requires 1x')
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        (self.path / 'control.txt').write_text('run-a 5 10 1 75\n')
        self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False})
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 6 1 0 1\n')

    def test_viewer_capacity_is_bounded(self):
        for _ in range(16):
            self.assertTrue(self.spool.viewers.acquire(blocking=False))
        self.assertFalse(self.spool.viewers.acquire(blocking=False))


if __name__ == '__main__':
    unittest.main()
