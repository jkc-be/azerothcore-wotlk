import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('bridge', Path(__file__).parents[1] / 'bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class Controls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.spool = bridge.Spool(self.path, 'test-token', follow=False)
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
        restarted = bridge.Spool(self.path, 'test-token', follow=False)
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

    def test_observer_mode_is_validated_retained_and_changeable_under_gm_lock(self):
        self.snapshot.update(observers=1, expectedBots=1, maxBots=100, observerMode=0)
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        for mode in (-1, 3, True, 1.0, '1', None):
            with self.assertRaises(ValueError):
                self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'observerMode': mode})
        self.assertFalse((self.path / 'control.txt').exists())
        # The speed lock does not block a mode change while observers are connected.
        self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'observerMode': 2})
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 6 1 0 1 2\n')
        # A later population request keeps the pending mode; a bridge restart reads it back from the mailbox.
        restarted = bridge.Spool(self.path, 'test-token', follow=False)
        restarted.control({'run': 'run-a', 'speed': 1, 'paused': False, 'bots': 3})
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 7 1 0 3 2\n')
        self.snapshot.update(controlSeq=7, observerMode=2)
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        restarted.control({'run': 'run-a', 'speed': 1, 'paused': False, 'observerMode': 0})
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 8 1 0 3 0\n')

    def test_old_world_rejects_observer_mode_and_gets_no_extra_field(self):
        self.snapshot.update(expectedBots=1, maxBots=100)
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        with self.assertRaises(ValueError):
            self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'observerMode': 1})
        self.assertFalse((self.path / 'control.txt').exists())
        self.spool.control({'run': 'run-a', 'speed': 2, 'paused': False})
        self.assertEqual((self.path / 'control.txt').read_text(), 'run-a 6 2 0 1\n')

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

    def test_python_feed_cannot_create_a_control_mailbox(self):
        (self.path / 'latest.json').write_text(json.dumps({'source': 'python-api', 'run': 'run-a'}))
        with self.assertRaisesRegex(ValueError, 'read-only'):
            self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'bots': 0})
        self.assertFalse((self.path / 'control.txt').exists())

    def test_history_tail_ignores_partial_records_and_bounds_reads(self):
        path = self.path / 'snapshots.ndjson'
        lines = [json.dumps({'seq': i}) + '\n' for i in range(10)]
        path.write_text(''.join(lines) + '{"seq": 10')
        self.assertEqual(bridge.journal_tail(path, 3, 4096), [{'seq': i} for i in (7, 8, 9)])
        self.assertEqual(bridge.journal_tail(path, 100, 35), [{'seq': 8}, {'seq': 9}])
        path.write_text('null\n[]\nbad\n' + lines[0])
        self.assertEqual(bridge.journal_tail(path, 100, 4096), [{'seq': 0}])


class AdaptiveSpeed(unittest.TestCase):
    def setUp(self):
        clock = patch.object(bridge.time, 'monotonic', return_value=0)
        clock.start()
        self.addCleanup(clock.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.spool = bridge.Spool(self.path, 'test-token', follow=False)
        self.addCleanup(self.spool.close)
        self.frame = {'run': 'adaptive', 'seq': 0, 'controlSeq': 0, 'requestedSpeed': 1,
                      'backlogMs': 0, 'fault': '', 'ready': True, 'paused': False,
                      'expectedBots': 30, 'maxBots': 30, 'populationPending': False}
        self.publish()

    def publish(self):
        (self.path / 'latest.json').write_text(json.dumps(self.frame))

    def enable(self, **extra):
        with patch.object(bridge.time, 'monotonic', return_value=0):
            return self.spool.control({'run': 'adaptive', 'speed': 'max', 'paused': False, **extra})

    def advance(self, now, acknowledge=True, **extra):
        if acknowledge:
            fields = (self.path / 'control.txt').read_text().split()
            self.frame.update(controlSeq=int(fields[1]), requestedSpeed=float(fields[2]),
                              paused=bool(int(fields[3])), expectedBots=int(fields[4]))
        self.frame.update(extra)
        self.frame['seq'] += 1
        self.publish()
        self.spool.adjust_speed(now)

    def requested(self):
        return float((self.path / 'control.txt').read_text().split()[2])

    def sample(self, start, end, backlog=lambda now: 0):
        for tick in range(round(start * 4), round(end * 4) + 1):
            self.advance(tick / 4, backlogMs=backlog(tick / 4))

    def test_max_starts_at_one_then_keeps_probing_with_stable_backlog(self):
        self.frame['requestedSpeed'] = 10
        self.publish()
        self.enable()
        self.assertEqual(self.requested(), 1)
        self.sample(0, 4.75, lambda now: 60)
        self.assertEqual(self.requested(), 1)
        self.sample(5, 5, lambda now: 60)
        self.assertEqual(self.requested(), 2)
        self.sample(5.25, 10.25, lambda now: 60)
        self.assertEqual(self.requested(), 5)
        self.sample(10.5, 15.5, lambda now: 60)
        self.assertEqual(self.requested(), 10)
        self.sample(15.75, 19, lambda now: 60)
        self.assertIn('highest available', self.spool.snapshot()['speedControl']['status'])

    def test_repeated_brief_spikes_do_not_throttle_or_block_probes(self):
        self.enable()
        self.sample(0, 5)
        self.assertEqual(self.requested(), 2)
        self.sample(5.25, 10.25, lambda now: 2000 if now % 1 == 0 else 0)
        self.assertEqual(self.requested(), 5)
        self.sample(10.5, 15.5, lambda now: 2000 if now % 1 == 0 else 0)
        self.assertEqual(self.requested(), 10)

    def test_sustained_growth_throttles_then_reprobes_after_recovery(self):
        self.enable()
        self.sample(0, 5)
        self.sample(5.25, 8, lambda now: (now - 5.25) * 100)
        self.assertEqual(self.requested(), 2)
        self.sample(8.25, 8.25, lambda now: 300)
        self.assertEqual(self.requested(), 1)
        self.sample(8.5, 13.25)
        self.assertEqual(self.requested(), 1)
        self.sample(13.5, 13.5)
        self.assertEqual(self.requested(), 2)

    def test_falling_backlog_drains_without_throttling_then_probes(self):
        self.enable()
        self.sample(0, 5)
        self.sample(5.25, 13.25, lambda now: 1000 - (now - 5.25) * 100)
        self.assertEqual(self.requested(), 2)
        self.assertIn('drains', self.spool.max_speed.status)
        self.sample(13.5, 20.5, lambda now: max(0, 1000 - (now - 5.25) * 100))
        self.assertEqual(self.requested(), 5)

    def test_flat_backlog_above_target_still_probes_instead_of_getting_stuck(self):
        self.enable()
        self.sample(0, 5, lambda now: 200)
        self.assertEqual(self.requested(), 2)

    def test_decimal_max_visits_intermediate_rates_and_stops_at_ten(self):
        self.frame['speedStep'] = 0.1
        self.publish()
        self.enable()
        rates = {self.requested()}
        for tick in range(2001):
            self.advance(tick / 4)
            rates.add(self.requested())
        self.assertEqual(rates, set(bridge.DECIMAL_SPEEDS))
        self.assertEqual(self.requested(), 10)

    def test_decimal_manual_control_and_pause_preserve_exact_rate(self):
        self.frame['speedStep'] = 0.1
        self.publish()
        for speed in (2.1, 3.2):
            for paused in (False, True):
                self.spool.control({'run': 'adaptive', 'speed': speed, 'paused': paused})
                self.assertEqual(self.requested(), speed)
                self.assertEqual((self.path / 'control.txt').read_text().split()[3], str(int(paused)))

    def test_old_world_rejects_decimal_rates_without_writing(self):
        for speed in (1.1, 2.1, 3.2, 3):
            with self.subTest(speed=speed), self.assertRaisesRegex(ValueError, 'Update the worldserver'):
                self.spool.control({'run': 'adaptive', 'speed': speed, 'paused': False})
        self.assertFalse((self.path / 'control.txt').exists())
        self.spool.control({'run': 'adaptive', 'speed': 2.0, 'paused': False})
        self.assertEqual((self.path / 'control.txt').read_text().split()[2], '2')

    def test_invalid_decimal_rates_do_not_reach_mailbox(self):
        self.frame['speedStep'] = 0.1
        self.publish()
        for speed in (0.9, 10.1, 2.15, float('nan'), float('inf'), True, '2.1'):
            with self.subTest(speed=speed), self.assertRaises(ValueError):
                self.spool.control({'run': 'adaptive', 'speed': speed, 'paused': False})
        self.assertFalse((self.path / 'control.txt').exists())

    def test_decimal_capacity_drop_uses_larger_backoff_and_recovers(self):
        self.frame['speedStep'] = 0.1
        self.publish()
        self.enable()
        backlog, rates, debts = 0, [], []
        for tick in range(1601):
            capacity = 3.25 if tick < 800 else 1.7
            backlog = max(0, backlog + (self.requested() - capacity) * 250)
            self.advance(tick / 4, backlogMs=backlog)
            rates.append(self.requested())
            debts.append(backlog)
        self.assertIn(2.1, rates[:800])
        self.assertIn(3.2, rates[:800])
        self.assertLess(max(debts), 15000)
        self.assertLessEqual(max(rates[1000:]), 1.8)
        self.assertGreaterEqual(min(rates[1000:]), 1.5)
        self.assertIn(0, debts[1000:])

    def test_waits_for_ack_and_stops_on_rejected_or_external_control(self):
        self.enable()
        self.advance(10, acknowledge=False)
        self.assertEqual(self.requested(), 1)
        self.assertIn('acknowledgement', self.spool.max_speed.status)
        self.advance(11, controlError='GM POV requires 1x')
        self.assertIsNone(self.spool.max_speed)
        self.frame['controlError'] = ''
        self.publish()
        self.enable()
        fields = (self.path / 'control.txt').read_text().split()
        fields[1] = str(int(fields[1]) + 1)
        (self.path / 'control.txt').write_text(' '.join(fields) + '\n')
        self.spool.adjust_speed(12)
        self.assertIsNone(self.spool.max_speed)
        self.assertEqual((self.path / 'control.txt').read_text(), ' '.join(fields) + '\n')

    def test_pause_and_population_preserve_max_and_pending_bot_target(self):
        self.enable(bots=12)
        result = self.spool.control({'run': 'adaptive', 'speed': 'max', 'paused': True})
        self.assertEqual((self.path / 'control.txt').read_text(), f"adaptive {result['sequence']} 1 1 12\n")
        self.advance(1)
        self.assertEqual(self.spool.max_speed.status, 'Paused')
        self.spool.control({'run': 'adaptive', 'speed': 'max', 'paused': False})
        self.advance(2, populationPending=True)
        self.advance(20, populationPending=True)
        self.assertEqual(self.requested(), 1)
        self.advance(21, populationPending=False)
        self.sample(21.25, 26)
        self.assertEqual(self.requested(), 2)
        self.assertEqual((self.path / 'control.txt').read_text().split()[4], '12')

    def test_manual_selection_cancels_max(self):
        self.enable()
        self.spool.control({'run': 'adaptive', 'speed': 5, 'paused': False})
        self.spool.adjust_speed(60)
        self.assertEqual(self.requested(), 5)
        self.assertEqual(self.spool.snapshot()['speedControl']['mode'], 'manual')

    def test_stale_frames_never_trigger_a_probe(self):
        self.enable()
        self.advance(0)
        self.spool.adjust_speed(10)
        self.assertEqual(self.requested(), 1)
        self.assertIn('fresh telemetry', self.spool.max_speed.status)
        self.advance(11)
        self.assertEqual(self.requested(), 1)

    def test_gap_between_new_frames_restarts_measurement(self):
        self.enable()
        self.sample(0, 2)
        self.advance(10)
        self.assertEqual(self.requested(), 1)
        self.sample(10.25, 14.75)
        self.assertEqual(self.requested(), 1)
        self.advance(15)
        self.assertEqual(self.requested(), 2)

    def test_unachievable_limit_reports_floor_instead_of_pausing(self):
        self.enable()
        self.sample(0, 3, lambda now: 200 + now * 100)
        self.assertEqual(self.requested(), 1)
        self.assertIn('At 1×', self.spool.max_speed.status)
        self.assertEqual((self.path / 'control.txt').read_text().split()[3], '0')

    def test_run_change_observer_fault_and_read_only_feed_cancel_max(self):
        for changed in ({'run': 'new'}, {'observers': 1}, {'baseline': True}, {'fault': 'journal_failure'},
                        {'completed': True}, {'source': 'python-api'}, {'readOnly': True}):
            with self.subTest(changed=changed):
                self.enable()
                before = (self.path / 'control.txt').read_text()
                original = dict(self.frame)
                self.advance(1, **changed)
                self.assertIsNone(self.spool.max_speed)
                self.assertEqual((self.path / 'control.txt').read_text(), before)
                self.frame = original
                self.publish()

    def test_threshold_validation_and_default(self):
        for value in (0, -1, 9, 60001, True, 100.5, '100', None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.enable(backlogLimitMs=value)
        self.assertFalse((self.path / 'control.txt').exists())
        self.enable()
        self.assertEqual(self.spool.snapshot()['speedControl']['backlogLimitMs'], 100)
        self.enable(backlogLimitMs=250)
        self.assertEqual(self.spool.max_speed.limit, 250)
        with self.assertRaises(ValueError):
            self.spool.control({'run': 'adaptive', 'speed': 2, 'paused': False, 'backlogLimitMs': 100})

    def test_max_rejects_unavailable_or_invalid_telemetry_without_writing(self):
        for changed in ({'observers': 1}, {'baseline': True}, {'backlogMs': float('nan')},
                        {'backlogMs': -1}, {'backlogMs': None}, {'requestedSpeed': 3}):
            original = dict(self.frame)
            self.frame.update(changed)
            self.publish()
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                self.enable()
            self.assertFalse((self.path / 'control.txt').exists())
            self.frame = original
            self.publish()

    def test_capacity_drop_recovers_and_continues_probing_without_runaway_debt(self):
        self.enable()
        backlog, speeds, debts = 0, [], []
        for tick in range(1, 481):
            speed = self.requested()
            capacity = 7 if tick <= 240 else 3
            backlog = max(0, backlog + (speed - capacity) * 250)
            self.advance(tick / 4, backlogMs=backlog)
            speeds.append(self.requested())
            debts.append(backlog)
        # Faster probes are expected throughout, so do not assume the final frame is at the floor.
        self.assertGreater(speeds[60:240].count(5), speeds[60:240].count(10))
        self.assertEqual(set(speeds[280:]), {2, 5})
        self.assertGreater(speeds[280:].count(2), speeds[280:].count(5))
        self.assertGreater(debts[280:].count(0), 20)
        # Two successive three-second observations cover the drop from 10x through 5x to 2x.
        self.assertLess(max(debts), 20000)
        self.assertLess(max(debts[360:]), 7000)


class EventFollower(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'events.ndjson'

    @staticmethod
    def record(seq, kind, **extra):
        return json.dumps({'run': 'run-a', 'seq': seq, 'simMs': seq * 10, 'bot': 'b', 'kind': kind, **extra}) + '\n'

    def test_missing_journal_then_incremental_follow_without_rescans(self):
        tail = bridge.EventTail(self.path, recent=3, progression=4)
        tail.poll()
        self.assertEqual(tail.events('progression'), [])
        self.assertEqual(tail.stats()['total'], 0)
        with self.path.open('w') as journal:
            journal.write(self.record(1, 'xp', value=5) + self.record(2, 'position') + self.record(3, 'position'))
        tail.poll()
        # The seed batch restores display records but does not count as appended work.
        self.assertEqual([event['seq'] for event in tail.events('progression')], [1])
        self.assertEqual(tail.stats()['total'], 0)
        with self.path.open('a') as journal:
            journal.write(self.record(4, 'position') + self.record(5, 'quest_reward', value=7) + '{"seq": 6, "ki')
        tail.poll()
        self.assertEqual([event['seq'] for event in tail.events('progression')], [1, 5])
        self.assertEqual([event['seq'] for event in tail.events('recent')], [3, 4, 5])
        stats = tail.stats()
        self.assertEqual((stats['total'], stats['kinds'], stats['recent']),
                         (2, {'position': 1, 'quest_reward': 1}, {'position': 1, 'quest_reward': 1}))
        with self.path.open('a') as journal:
            journal.write('nd": "death"}\nnull\n[]\n' + self.record(7, 'bot_action', detail='loot'))
        tail.poll()
        self.assertEqual([event['seq'] for event in tail.events('progression')], [1, 5, 6, 7])
        self.assertEqual(tail.stats()['kinds'], {'position': 1, 'quest_reward': 1, 'death': 1, 'bot_action': 1})

    def test_replaced_journal_restarts_from_its_tail(self):
        tail = bridge.EventTail(self.path)
        self.path.write_text(self.record(1, 'xp') * 3)
        tail.poll()
        self.assertEqual(len(tail.events('recent')), 3)
        self.path.unlink()
        self.path.write_text(self.record(9, 'death'))
        tail.poll()
        self.assertEqual([event['seq'] for event in tail.events('recent')][-1], 9)
        self.assertEqual(tail.stats()['total'], 0)

    def test_large_seed_skips_the_partial_first_line(self):
        with self.path.open('w') as journal:
            for seq in range(1, 6000):
                journal.write(self.record(seq, 'position', detail='x' * 60))
            journal.write(self.record(6000, 'xp'))
        tail = bridge.EventTail(self.path)
        tail.poll()
        recent = tail.events('recent')
        self.assertEqual(recent[-1]['seq'], 6000)
        self.assertEqual(len(recent), 500)
        self.assertEqual(tail.events('progression'), [recent[-1]])


if __name__ == '__main__':
    unittest.main()
