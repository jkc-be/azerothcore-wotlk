import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('bridge', Path(__file__).parents[1] / 'bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class TokenFile(unittest.TestCase):
    def test_creates_nested_token_file_and_reuses_it(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'nested' / 'token'
            token = bridge.load_token(path)
            self.assertGreaterEqual(len(token), 32)
            self.assertEqual(path.read_text().strip(), token)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(bridge.load_token(path), token)

    def test_rejects_short_existing_token(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'token'
            path.write_text('too-short')
            with self.assertRaisesRegex(ValueError, 'at least 32 characters'):
                bridge.load_token(path)


class Authentication(unittest.TestCase):
    def test_non_ascii_credentials_authenticate_or_return_401_without_disconnect(self):
        import urllib.error
        import urllib.request

        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            (path / 'latest.json').write_text(json.dumps({'run': 'auth-test', 'seq': 1}))
            server = bridge.ThreadingHTTPServer(('127.0.0.1', 0), bridge.Handler)
            server.daemon_threads = True
            server.spool = bridge.Spool(path, 'test-token', follow=False)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                for token in ('a' * 32, 'a' * 32 + '\u00e9\u00f1'):
                    server.spool.token = token
                    request = urllib.request.Request(f'http://127.0.0.1:{server.server_port}/api/snapshot',
                                                     headers={'Authorization': 'Bearer ' + token})
                    with urllib.request.urlopen(request, timeout=5) as response:
                        self.assertEqual(response.status, 200)
                        self.assertEqual(json.load(response)['run'], 'auth-test')
                    for method in ('GET', 'POST'):
                        for supplied in ('', 'Bearer wrong', 'Bearer ' + token + '\u00e9'):
                            with self.subTest(token_ascii=token.isascii(), method=method, supplied=supplied):
                                request = urllib.request.Request(
                                    f'http://127.0.0.1:{server.server_port}/api/snapshot', method=method,
                                    headers={'Authorization': supplied})
                                with self.assertRaises(urllib.error.HTTPError) as error:
                                    urllib.request.urlopen(request, timeout=5)
                                self.assertEqual(error.exception.code, 401)
                                error.exception.close()
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
                server.spool.close()


class Controls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.spool = bridge.Spool(self.path, 'test-token', follow=False)
        self.snapshot = {'run': 'run-a', 'controlSeq': 5, 'fault': ''}
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))

    def test_stopped_world_is_recorded_read_only_until_fresh_telemetry_arrives(self):
        path = self.path / 'latest.json'
        old = time.time() - 30
        os.utime(path, (old, old))
        recorded = self.spool.snapshot()
        self.assertTrue(recorded['telemetryStale'])
        self.assertTrue(recorded['readOnly'])
        with self.assertRaisesRegex(ValueError, 'read-only'):
            self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False})
        self.assertFalse((self.path / 'control.txt').exists())
        self.assertEqual(json.loads(path.read_text()), self.snapshot)

        path.write_text(json.dumps(self.snapshot))
        fresh = self.spool.snapshot()
        self.assertNotIn('telemetryStale', fresh)
        self.assertNotIn('readOnly', fresh)
        self.assertEqual(self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False})['status'], 'accepted')

    def test_ordinary_live_feed_stays_read_only_and_does_not_invent_totals(self):
        self.snapshot.update(source='alles-live', readOnly=True, simMs=1000, bots=[
            {'name': 'Humana', 'level': 2, 'health': 30, 'maxHealth': 40, 'activity': 'moving', 'money': 12}])
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        self.assertNotIn('telemetryStale', self.spool.snapshot())
        self.assertTrue(self.spool.snapshot()['readOnly'])
        with self.assertRaisesRegex(ValueError, 'read-only'):
            self.spool.control({'run': 'run-a', 'speed': 1, 'paused': True})
        summary = bridge.summarize(self.snapshot)
        self.assertIsNone(summary['xp'])
        self.assertIsNone(summary['quests'])
        self.assertIsNone(summary['deaths'])
        self.assertEqual(summary['money'], 12)

    def test_live_feed_summary_totals_only_reported_counters_and_carries_interpreter_figures(self):
        frame = {'run': 'r', 'simMs': 1000, 'seq': 1, 'source': 'alles-live', 'activeBots': 1,
                 'interpreter': {'connected': False, 'usedRequests': 7, 'maxRequests': 100, 'remainingRequests': 93,
                                 'modelMemories': 4, 'fallbackMemories': 9, 'invalidResults': 0},
                 'bots': [{'level': 2, 'memoryCount': 5, 'pendingPerceptions': 1, 'earnedXp': 30, 'deaths': 1,
                           'questCompletions': 0},
                          {'level': 3, 'memoryCount': 7}]}
        point = bridge.summarize(frame)
        self.assertEqual(point['memories'], 12)
        self.assertEqual(point['pendingPerceptions'], 1)
        self.assertEqual(point['xp'], 30)
        self.assertEqual(point['deaths'], 1)
        self.assertEqual(point['quests'], 0)
        self.assertEqual(point['modelMemories'], 4)
        self.assertEqual(point['fallbackMemories'], 9)
        self.assertEqual(point['usedRequests'], 7)
        self.assertEqual(point['remainingRequests'], 93)
        self.assertEqual(point['workerConnected'], 0)
        self.assertEqual(point['active'], 1)
        empty = bridge.summarize({'simMs': 0, 'bots': []})
        self.assertIsNone(empty['xp'])
        self.assertIsNone(empty['memories'])
        self.assertIsNone(empty['workerConnected'])
        # The connected share averages like the other gauges when buckets fold.
        merged = bridge.combine_points(dict(point, bucket=0, samples=1),
                                       dict(point, bucket=0, samples=1, workerConnected=1, seq=2))
        self.assertEqual(merged['workerConnected'], 0.5)
        self.assertEqual(merged['memories'], 12)

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


class LiveRealm(unittest.TestCase):
    """An ordinary realm publishes every boot into the same directory under a new run id."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)

    def record(self, run, seq, kind='xp'):
        return json.dumps({'run': run, 'seq': seq, 'simMs': seq * 1000, 'bot': 'b', 'kind': kind, 'value': 1}) + '\n'

    def test_run_change_starts_fresh_journal_tiers_that_keep_other_runs_out(self):
        (self.path / 'latest.json').write_text(json.dumps({'run': 'run-a', 'seq': 3, 'simMs': 3000, 'bots': []}))
        with (self.path / 'events.ndjson').open('w') as journal:
            journal.writelines(self.record('run-a', seq) for seq in (1, 2, 3))
        with (self.path / 'events-rollup.ndjson').open('w') as rollup:
            rollup.write(json.dumps({'bucket': 0, 'samples': 1, 'kinds': {'xp': 1}, 'run': 'run-a', 'seq': 1}) + '\n')
            rollup.write(json.dumps({'bucket': 0, 'samples': 1, 'kinds': {'xp': 1}, 'run': 'run-b', 'seq': 1}) + '\n')
            rollup.write(json.dumps({'bucket': 300000, 'samples': 1, 'kinds': {'xp': 1}}) + '\n')
        spool = bridge.Spool(self.path, 'test-token', follow=False)
        self.addCleanup(spool.close)
        self.assertEqual(spool.run, 'run-a')
        self.assertEqual(spool.tail.rates.stored, 2)  # run-a and the legacy point without a run
        spool.tail.poll()
        self.assertEqual([event['seq'] for event in spool.tail.events('progression')], [1, 2, 3])
        self.assertEqual(spool.tail.progression_seq, 3)
        self.assertFalse(spool.rotate_run())

        # A new world boot: the same files carry the new run's records after the world archived the old ones.
        with (self.path / 'events.ndjson').open('a') as journal:
            journal.writelines(self.record('run-b', seq) for seq in (1, 2))
        (self.path / 'latest.json').write_text(json.dumps({'run': 'run-b', 'seq': 1, 'simMs': 1000, 'bots': []}))
        self.assertEqual(spool.snapshot()['run'], 'run-b')
        self.assertTrue(spool.rotate_run())
        self.assertEqual(spool.run, 'run-b')
        self.assertEqual(spool.tail.rates.stored, 2)  # run-b and the legacy point; run-a is gone
        self.assertEqual(spool.tail.progression_seq, -1)
        spool.tail.poll()
        self.assertEqual([(event['run'], event['seq']) for event in spool.tail.events('progression')],
                         [('run-b', 1), ('run-b', 2)])
        lines = [json.loads(line) for line in (self.path / 'progression.ndjson').read_text().splitlines()]
        self.assertEqual([(line['run'], line['seq']) for line in lines],
                         [('run-a', 1), ('run-a', 2), ('run-a', 3), ('run-b', 1), ('run-b', 2)])
        self.assertFalse(spool.rotate_run())

    def test_run_change_without_snapshot_requests_keeps_recording(self):
        latest = self.path / 'latest.json'
        latest.write_text(json.dumps({'run': 'run-a'}))
        journal = self.path / 'events.ndjson'
        journal.write_text(self.record('run-a', 1))
        spool = bridge.Spool(self.path, 'test-token', follow=False)
        self.addCleanup(spool.close)
        spool.tail.poll()
        journal.rename(self.path / 'previous-events')
        latest.unlink()
        self.assertFalse(spool.rotate_run())
        self.assertEqual(spool.run, 'run-a')
        latest.write_text('incomplete')
        self.assertFalse(spool.rotate_run())

        journal.write_text(self.record('run-b', 1))
        latest.write_text(json.dumps({'run': 'run-b'}))
        # This is the follower's own rotation path. No HTTP/snapshot request wakes it up.
        self.assertTrue(spool.rotate_run())
        spool.tail.poll()
        with journal.open('a') as output:
            output.write(self.record('run-b', 2))
        spool.tail.poll()
        self.assertEqual(spool.run, 'run-b')
        self.assertEqual([(event['run'], event['seq']) for event in spool.tail.events('progression')],
                         [('run-b', 1), ('run-b', 2)])
        self.assertEqual(spool.tail.stats()['kinds'], {'xp': 1})

    def test_worker_log_tail_is_parsed_into_jobs_and_a_summary(self):
        log = self.path / 'alles-interpreter.log'
        log.write_text(
            '2026/09/07 14:27:57 worker: dial tcp 127.0.0.1:8779: connect: connection refused\n'
            '2026/09/07 14:28:12 connected model=qwen3:8b-q8_0 charged_requests=98\n'
            '2026/09/07 14:28:20 job=73b84fd4ec401ee3 status=applied memories=1 prompt_tokens=1427 '
            'completion_tokens=69 latency_ms=1556\n'
            '2026/09/07 14:28:30 job=95c5c815b32a3daf status=rejected memories=0 prompt_tokens=1400 '
            'completion_tokens=44 latency_ms=1200\n'
            '2026/09/07 14:28:40 job=249f638e8f067812 failed: HTTP permit denied\n'
            '2026/09/07 14:28:41 pilot request budget exhausted; gameplay continues with fallback\n'
            '2026/09/07 14:29:00 conversation=conversation-1788794048535-60 status=accepted action=wave '
            'reply=true prompt_tokens=761 completion_tokens=40 latency_ms=800\n'
            '2026/09/07 14:29:01 conversation=conversation-1788794051534-61 status=accepted action=none '
            'reply=false prompt_tokens=841 completion_tokens=22 latency_ms=400\n'
            '2026/09/07 14:29:02 conversation=conversation-1788794051534-62 failed: context deadline exceeded\n')
        result = bridge.worker_log(log)
        self.assertTrue(result['available'])
        self.assertEqual(len(result['lines']), 9)
        self.assertEqual([turn['status'] for turn in result['conversations']], ['accepted', 'accepted', 'failed'])
        self.assertEqual(result['conversations'][0], {
            'time': '2026/09/07 14:29:00', 'conversation': 'conversation-1788794048535-60', 'status': 'accepted',
            'action': 'wave', 'reply': True, 'promptTokens': 761, 'completionTokens': 40, 'latencyMs': 800})
        talk = result['summary']['conversations']
        self.assertEqual((talk['turns'], talk['accepted'], talk['otherStatus'], talk['failed'], talk['replies']),
                         (3, 2, 0, 1, 1))
        self.assertEqual(talk['actions'], {'wave': 1, 'none': 1})
        self.assertEqual((talk['meanLatencyMs'], talk['maxLatencyMs']), (600, 800))
        self.assertEqual((talk['promptTokens'], talk['completionTokens']), (1602, 62))
        self.assertEqual(result['summary']['lastError'], {'time': '2026/09/07 14:29:02',
                                                          'text': 'context deadline exceeded'})
        self.assertEqual(result['summary']['budgetExhaustedAt'], '2026/09/07 14:28:41')
        log.write_text(log.read_text() + '2026/09/07 14:30:00 connected model=qwen3:8b-q8_0 charged_requests=0\n')
        self.assertFalse(bridge.worker_log(log)['summary']['budgetExhausted'])
        # Job lines are unaffected by the conversation lines.
        result = bridge.worker_log(log)
        self.assertEqual([job['status'] for job in result['jobs']], ['applied', 'rejected', 'failed'])
        self.assertEqual(result['jobs'][0], {'time': '2026/09/07 14:28:20', 'job': '73b84fd4ec401ee3',
                                             'status': 'applied', 'memories': 1, 'promptTokens': 1427,
                                             'completionTokens': 69, 'latencyMs': 1556})
        self.assertEqual(result['jobs'][2]['error'], 'HTTP permit denied')
        summary = result['summary']
        self.assertEqual((summary['jobs'], summary['applied'], summary['otherStatus'], summary['failed']),
                         (3, 1, 1, 1))
        self.assertEqual(summary['meanLatencyMs'], 1378)
        self.assertEqual(summary['maxLatencyMs'], 1556)
        self.assertEqual(summary['promptTokens'], 2827)
        self.assertEqual(summary['completionTokens'], 113)
        self.assertEqual(summary['lastConnected'], '2026/09/07 14:30:00')
        self.assertFalse(bridge.worker_log(self.path / 'missing.log')['available'])
        self.assertFalse(bridge.worker_log(None)['available'])
        self.assertEqual(bridge.worker_log(log, limit=2)['lines'],
                         result['lines'][-2:])


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
        self.addCleanup(tail.close)
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

    def test_rotated_journal_is_drained_before_its_successor_is_read_from_the_start(self):
        tail = bridge.EventTail(self.path)
        self.addCleanup(tail.close)
        self.path.write_text(self.record(1, 'xp') * 3)
        tail.poll()
        self.assertEqual(len(tail.events('recent')), 3)
        # The world appends, closes and renames the segment, then opens a fresh journal, all between polls.
        with self.path.open('a') as journal:
            journal.write(self.record(4, 'death'))
        self.path.rename(self.path.with_name('events.000001.ndjson'))
        self.path.write_text(self.record(5, 'position') + self.record(6, 'quest_reward'))
        tail.poll()
        self.assertEqual([event['seq'] for event in tail.events('recent')][-3:], [4, 5, 6])
        self.assertEqual(tail.stats()['kinds'], {'death': 1, 'position': 1, 'quest_reward': 1})
        # A journal that vanishes is simply followed again once it reappears.
        self.path.unlink()
        tail.poll()
        self.path.write_text(self.record(9, 'death'))
        tail.poll()
        self.assertEqual([event['seq'] for event in tail.events('recent')][-1], 9)
        self.assertEqual(tail.stats()['kinds']['death'], 2)

    def test_large_seed_skips_the_partial_first_line(self):
        with self.path.open('w') as journal:
            for seq in range(1, 6000):
                journal.write(self.record(seq, 'position', detail='x' * 60))
            journal.write(self.record(6000, 'xp'))
        tail = bridge.EventTail(self.path)
        self.addCleanup(tail.close)
        tail.poll()
        recent = tail.events('recent')
        self.assertEqual(recent[-1]['seq'], 6000)
        self.assertEqual(len(recent), 500)
        self.assertEqual(tail.events('progression'), [recent[-1]])

    def test_long_term_counts_per_bucket_and_progression_survive_a_restart(self):
        tail = bridge.EventTail(self.path)
        self.path.write_text(self.record(1, 'xp'))
        tail.poll()  # seed: displayed and kept in progression.ndjson, but not counted
        minute = bridge.BUCKET_MS
        with self.path.open('a') as journal:
            journal.write(self.record(2, 'position') + self.record(3, 'quest_reward')
                          + json.dumps({'run': 'run-a', 'seq': 4, 'simMs': minute + 5, 'kind': 'death'}) + '\n')
        tail.poll()
        points = tail.long_term()['points']
        self.assertEqual([(point['bucket'], point['kinds']) for point in points],
                         [(0, {'position': 1, 'quest_reward': 1}), (minute, {'death': 1})])
        self.assertTrue(points[-1]['partial'])
        tail.close()
        progression = self.path.with_name('progression.ndjson').read_text().splitlines()
        self.assertEqual([json.loads(line)['seq'] for line in progression], [1, 3, 4])

        restarted = bridge.EventTail(self.path)
        self.addCleanup(restarted.close)
        restarted.poll()  # the seed batch repeats records already stored; none is written twice
        with self.path.open('a') as journal:
            journal.write(json.dumps({'run': 'run-a', 'seq': 5, 'simMs': minute + 9, 'kind': 'level'}) + '\n')
        restarted.poll()
        progression = self.path.with_name('progression.ndjson').read_text().splitlines()
        self.assertEqual([json.loads(line)['seq'] for line in progression], [1, 3, 4, 5])
        points = restarted.long_term()['points']
        self.assertEqual([(point['bucket'], point['kinds']) for point in points],
                         [(0, {'position': 1, 'quest_reward': 1}), (minute, {'death': 1}), (minute, {'level': 1})])
        restarted.close()
        # The bucket flushed at shutdown and the one completed afterwards merge on the next load.
        reloaded = bridge.EventTail(self.path)
        self.addCleanup(reloaded.close)
        self.assertEqual([(point['bucket'], point['kinds']) for point in reloaded.long_term()['points']],
                         [(0, {'position': 1, 'quest_reward': 1}), (minute, {'death': 1, 'level': 1})])


class TieredHistory(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)

    @staticmethod
    def frame(seq, sim_ms, level=1, health=100, paused=False, tick_us=1000):
        bots = [{'id': f'b{index}', 'map': 0, 'zone': 12, 'level': level + index, 'health': health,
                 'maxHealth': 100, 'earnedXp': seq * 10, 'questCompletions': seq, 'deaths': 0,
                 'money': 5, 'activity': 'combat' if index else 'idle', 'quests': [{'id': 1}]}
                for index in range(2)]
        return {'run': 'run-a', 'seq': seq, 'simMs': sim_ms, 'realMs': sim_ms, 'paused': paused,
                'maxTickUs': tick_us, 'backlogMs': 10, 'requestedSpeed': 2, 'achievedSpeed': 1.5,
                'onlineBots': 2, 'activeBots': 2, 'expectedBots': 2, 'bots': bots}

    def test_summary_matches_the_dashboard_model(self):
        point = bridge.summarize(self.frame(3, 1000, level=2, health=50))
        self.assertEqual((point['xp'], point['quests'], point['deaths'], point['questsActive'], point['money']),
                         (60, 6, 0, 2, 10))
        self.assertEqual((point['levels'], point['zones'], point['activity']['combat'], point['activity']['idle']),
                         ({'Level 2': 1, 'Level 3': 1}, {'0/12': 2}, 1, 1))
        self.assertEqual((point['meanLevel'], point['minLevel'], point['maxLevel'], point['meanHealth']),
                         (2.5, 2, 3, 50))
        self.assertEqual((point['tickMs'], point['pausedShare'], point['bucket'], point['samples']), (1, 0, 0, 1))

    def test_buckets_combine_gauges_counters_and_extremes_then_coarsen(self):
        a = bridge.summarize(self.frame(1, 0, level=1, tick_us=3000))
        b = bridge.summarize(self.frame(2, 1000, level=5, paused=True, tick_us=500))
        c = bridge.summarize(self.frame(3, 2000, level=3))
        merged = bridge.combine_points(a, b)
        self.assertEqual((merged['samples'], merged['seq'], merged['xp'], merged['minLevel'], merged['maxLevel']),
                         (2, 2, 40, 1, 6))
        self.assertEqual((merged['meanLevel'], merged['tickMs'], merged['pausedShare']), (3.5, 3, 0.5))
        self.assertEqual(merged['activity'], {'idle': 1, 'moving': 0, 'combat': 1, 'casting': 0, 'dead': 0})
        self.assertEqual(bridge.combine_points(b, a)['seq'], 2)  # order of arrival does not matter
        coarse = bridge.coarsen([a, b, c], 2, bridge.combine_points)
        self.assertEqual([(point['samples'], point['seq']) for point in coarse], [(2, 2), (1, 3)])
        self.assertEqual(merged['buckets'], 1)  # one bucket accumulating
        later = dict(c, bucket=bridge.BUCKET_MS)
        self.assertEqual(bridge.combine_points(merged, later)['buckets'], 2)  # two buckets folded
        counts = bridge.combine_counts({'bucket': 0, 'samples': 2, 'kinds': {'xp': 2}},
                                       {'bucket': bridge.BUCKET_MS, 'samples': 1, 'kinds': {'xp': 1, 'death': 1}})
        self.assertEqual((counts['buckets'], counts['samples'], counts['kinds']), (2, 3, {'xp': 3, 'death': 1}))
        self.assertEqual(bridge.coarsen([a, b, c], 3, bridge.combine_points), [a, b, c])

    def test_snapshot_follower_rolls_up_seeds_milestones_and_restores_without_double_counting(self):
        journal = self.path / 'snapshots.ndjson'
        bucket, milestone = bridge.BUCKET_MS, bridge.MILESTONE_MS
        with journal.open('w') as target:
            for seq, sim_ms in enumerate([0, 1000, bucket, bucket + 1000, milestone, milestone + 1000], start=1):
                target.write(json.dumps(self.frame(seq, sim_ms, level=seq)) + '\n')
        tail = bridge.SnapshotTail(journal)
        tail.poll()  # the seed is real history and enters the long-term tier
        points = tail.long_term()['points']
        self.assertEqual([(point['bucket'], point['samples'], point['seq']) for point in points],
                         [(0, 2, 2), (bucket, 2, 4), (milestone, 2, 6)])
        self.assertTrue(points[-1]['partial'])
        self.assertEqual(tail.milestone_count, 2)
        self.assertEqual([json.loads(line)['seq'] for line in (self.path / 'milestones.ndjson').read_text()
                          .splitlines()], [1, 5])
        tail.close()
        stored = [json.loads(line) for line in (self.path / 'rollup.ndjson').read_text().splitlines()]
        self.assertEqual([point['bucket'] for point in stored], [0, bucket, milestone])

        restarted = bridge.SnapshotTail(journal)
        self.addCleanup(restarted.close)
        restarted.poll()  # the seed repeats every stored frame
        with journal.open('a') as target:
            target.write(json.dumps(self.frame(7, milestone + 2000, level=7)) + '\n')
        restarted.poll()
        points = restarted.long_term()['points']
        self.assertEqual([(point['bucket'], point['samples'], point['seq']) for point in points],
                         [(0, 2, 2), (bucket, 2, 4), (milestone, 2, 6), (milestone, 1, 7)])
        self.assertEqual(restarted.milestone_count, 2)
        self.assertEqual(len(restarted.long_term(2)['points']), 2)

    def test_first_start_backfills_the_rollup_from_the_whole_journal_and_later_starts_seed_from_the_tail(self):
        journal = self.path / 'snapshots.ndjson'
        with journal.open('w') as target:
            for seq in range(1, 41):
                target.write(json.dumps(self.frame(seq, seq * bridge.BUCKET_MS)) + '\n')
        tail = bridge.SnapshotTail(journal, seed_bytes=2048)
        tail.poll()
        self.assertEqual(len(tail.long_term(1000)['points']), 40)
        tail.close()
        with journal.open('a') as target:
            for seq in range(41, 81):
                target.write(json.dumps(self.frame(seq, seq * bridge.BUCKET_MS)) + '\n')
        restarted = bridge.SnapshotTail(journal, seed_bytes=2048)
        self.addCleanup(restarted.close)
        restarted.poll()  # only the tail is seeded now; the gap stays visible instead of costing a full rescan
        points = restarted.long_term(1000)['points']
        self.assertLess(len(points), 80)
        self.assertEqual(points[-1]['seq'], 80)

    def test_prune_keeps_the_budget_and_never_deletes_unconsumed_or_live_files(self):
        spool = bridge.Spool(self.path, 'test-token', follow=False, retain_bytes=250)
        self.addCleanup(spool.close)
        record = json.dumps({'run': 'run-a', 'seq': 1, 'simMs': 0, 'kind': 'xp'}) + '\n'
        for index in (1, 2, 3):
            (self.path / f'events.{index:06d}.ndjson').write_text(record * 2)
        (self.path / 'events.ndjson').write_text(record * 20)
        spool.tail.poll()  # the follower holds the live file only
        spool.prune()
        self.assertEqual(sorted(path.name for path in self.path.glob('events*.ndjson')),
                         ['events.000002.ndjson', 'events.000003.ndjson', 'events.ndjson'])
        self.assertEqual(spool.pruned['events'], {'segments': 1, 'bytes': 2 * len(record)})
        # The follower still holds a segment that was rotated under it: it and newer segments are kept.
        (self.path / 'events.ndjson').rename(self.path / 'events.000004.ndjson')
        (self.path / 'events.000005.ndjson').write_text(record * 2)
        spool.prune()
        self.assertEqual(sorted(path.name for path in self.path.glob('events*.ndjson')),
                         ['events.000004.ndjson', 'events.000005.ndjson'])
        retention = spool.retention()
        self.assertTrue(retention['rotation'])
        self.assertEqual(retention['journals']['events']['segments'], 2)
        self.assertEqual(retention['journals']['events']['prunedSegments'], 3)
        self.assertEqual(retention['journals']['snapshots'],
                         {'currentBytes': 0, 'segments': 0, 'segmentBytes': 0, 'prunedSegments': 0,
                          'prunedBytes': 0})

    def test_history_and_exports_span_rotated_segments(self):
        import urllib.request
        for index in (1, 2):
            (self.path / f'snapshots.{index:06d}.ndjson').write_text(
                ''.join(json.dumps({'seq': index * 10 + offset}) + '\n' for offset in range(3)))
        (self.path / 'snapshots.ndjson').write_text('{"seq": 30}\n{"seq": 31')
        files = bridge.journal_files(self.path, 'snapshots')
        self.assertEqual([path.name for path in files],
                         ['snapshots.000001.ndjson', 'snapshots.000002.ndjson', 'snapshots.ndjson'])
        self.assertEqual([frame['seq'] for frame in bridge.journal_tail_files(files, 5, 4096)],
                         [11, 12, 20, 21, 22, 30][-5:])
        self.assertEqual([frame['seq'] for frame in bridge.journal_tail_files(files, 100, 40)], [22, 30])
        server = bridge.ThreadingHTTPServer(('127.0.0.1', 0), bridge.Handler)
        server.daemon_threads = True
        server.spool = bridge.Spool(self.path, 'test-token-of-sufficient-length', follow=False)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.addCleanup(server.spool.close)

        def get(path):
            request = urllib.request.Request(f'http://127.0.0.1:{server.server_port}{path}',
                                             headers={'Authorization': 'Bearer test-token-of-sufficient-length'})
            with urllib.request.urlopen(request) as response:
                return response.status, response.headers, response.read()

        status, headers, body = get('/api/export/snapshots.ndjson')
        expected = b''.join(path.read_bytes() for path in files)
        self.assertEqual((status, headers['Content-Length'], body), (200, str(len(expected)), expected))
        status, _, body = get('/api/history?scope=long-term&limit=5')
        self.assertEqual(json.loads(body), {'bucketMs': bridge.BUCKET_MS, 'points': []})
        status, _, body = get('/api/retention')
        self.assertEqual(json.loads(body)['journals']['snapshots']['segments'], 2)
        with self.assertRaises(urllib.error.HTTPError):
            get('/api/export/rollup.ndjson')


class FakeResponse:
    def __init__(self, payload):
        self.payload = payload

    def read(self):
        return json.dumps(self.payload).encode()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False


class MemoryInspection(unittest.TestCase):
    """The committed alles memory stores read through the mysql client, and out-of-game questions."""

    PASSWORD = 'p"a#s\\s'
    OWNERS = ('owner_kind\towner_id\tcommitted_revision\tname\tlevel\trace\tclass\tzone\tonline\tmemories\t'
              'perceptions\n0\t1176\t42\tHumana\t7\t1\t5\t12\t1\t2\t1\n1\t500\t3\tNULL\tNULL\tNULL\tNULL\tNULL\tNULL'
              '\t0\t0\n')
    ACTOR = ('committed_revision\tnext_memory_id\tnext_perception_id\tdecay_game_time_ms\tname\tlevel\trace\tclass'
             '\tzone\tonline\n42\t9\t4\t1000\tHumana\t7\t1\t5\t12\t1\n')
    MEMORIES = ('memory_id\tcontent_revision\tkind\tsubject_kind\tsubject_id\tsubject_name\tsource_kind\tsource_id'
                '\tsource_name\tclaim\tattribution\treported_depth\tconfidence\tsalience\tformed_game_time_ms'
                '\trecalled_game_time_ms\tdecay_game_time_ms\tformation_mode\n'
                '7\t1\t3\t1\t80999\tMangy Wolf\t0\t1176\tHumana\tMangy Wolf died after being attacked by Humana'
                '\t\tNULL\t0.9\t0.75\t1788790000000\t0\t1788790000000\t0\n'
                '5\t2\t0\tNULL\tNULL\t\t0\t1186\tHumanb\tthe mill\\tburned\tHumand\t2\t0.5\t0.4'
                '\t1788780000000\t1788781000000\t1788790000000\t3\n')
    PERCEPTIONS = ('perception_id\tkind\tsubject_name\tsource_name\tcomprehended\tlanguage\tgated_text\tplace'
                   '\tgame_time_ms\tself_context\n3\t0\t\tHumanb\t1\t7\thello there\tNorthshire Valley'
                   '\t1788790500000\tstanding\n')

    def fake_mysql(self):
        """A mysql stand-in that checks the private option file and answers by statement."""
        calls = []
        answers = {'AS perceptions FROM alles_actor': self.OWNERS, 'a.next_memory_id': self.ACTOR,
                   'FROM alles_memory WHERE': self.MEMORIES, 'FROM alles_perception WHERE': self.PERCEPTIONS}

        def run(command, capture_output, text, timeout):
            self.assertEqual(command[0], 'fake-mysql')
            option_file = Path(command[1][len('--defaults-extra-file='):])
            self.assertEqual(option_file.stat().st_mode & 0o777, 0o600)
            options = option_file.read_text()
            self.assertIn('password="p\\"a#s\\\\s"\n', options)
            self.assertIn('database="acore_characters"\n', options)
            self.assertNotIn(self.PASSWORD, ' '.join(command))
            calls.append((option_file, command[-1]))
            for needle, output in answers.items():
                if needle in command[-1]:
                    # Only owner 1176 has rows; any other owner gets the header alone.
                    owner = re.search(r'owner_id = (\d+)', command[-1])
                    if owner and owner[1] != '1176':
                        output = output.split('\n')[0] + '\n'
                    return SimpleNamespace(returncode=0, stdout=output, stderr='')
            return SimpleNamespace(returncode=1, stdout='', stderr='ERROR 1146 (42S02): missing table\n')

        return run, calls

    def database(self):
        run, calls = self.fake_mysql()
        return bridge.CharacterDatabase(f'127.0.0.1;3306;acore;{self.PASSWORD};acore_characters', client='fake-mysql',
                                        run=run), calls

    def test_owner_parsing_row_decoding_and_option_lines(self):
        self.assertEqual(bridge.parse_owner('player:1176'), (0, 1176))
        self.assertEqual(bridge.parse_owner('creature:7'), (1, 7))
        for bad in ('player:0', 'npc:3', 'player:-1', '1176', 'player:1; DROP TABLE x', ''):
            with self.assertRaises(ValueError):
                bridge.parse_owner(bad)
        rows = bridge.parse_tsv('a\tb\n1\tx\\ty\\nz\\\\\n2\tNULL\n')
        self.assertEqual(rows, [{'a': '1', 'b': 'x\ty\nz\\'}, {'a': '2', 'b': None}])
        self.assertEqual(bridge.parse_tsv(''), [])
        self.assertEqual(bridge.option_line('password', self.PASSWORD), 'password="p\\"a#s\\\\s"\n')
        with self.assertRaises(ValueError):
            bridge.CharacterDatabase('host;3306;user')

    def test_render_memory_matches_the_world(self):
        heard = {'kind': 'heard statement', 'source': {'name': 'Humana'}, 'attribution': 'Humanb',
                 'claim': 'the mill burned'}
        self.assertEqual(bridge.render_memory(heard), 'Humana told me Humanb reported that the mill burned')
        heard['attribution'] = 'Humana'
        self.assertEqual(bridge.render_memory(heard), 'Humana told me the mill burned')
        heard['source'] = {'name': ''}
        self.assertEqual(bridge.render_memory(heard), 'I heard the mill burned')
        death = {'kind': 'witnessed death', 'source': {'name': 'Humana'}, 'attribution': '', 'claim': 'a wolf died'}
        self.assertEqual(bridge.render_memory(death), 'I saw that a wolf died')
        met = {'kind': 'met', 'source': {'name': 'Humana'}, 'attribution': '', 'claim': 'I met Humand'}
        self.assertEqual(bridge.render_memory(met), 'I met Humand')

    def test_ranking_prefers_matching_claims_then_salience_without_duplicates(self):
        memories = [
            {'id': 1, 'text': 'I saw that Mangy Wolf died after being attacked by Humana', 'salience': 0.3},
            {'id': 2, 'text': 'I saw that Mangy Wolf died after being attacked by Humana', 'salience': 0.9},
            {'id': 3, 'text': 'Humanb told me the mill burned', 'salience': 0.8},
            {'id': 4, 'text': 'I met Humand', 'salience': 0.95},
            {'id': 5, 'text': 'x' * 600, 'salience': 1.0},
        ]
        ranked = bridge.rank_memories(memories, 'Which wolf died at the mill?', limit=3)
        self.assertEqual([memory['id'] for memory in ranked], [1, 3, 4])
        self.assertEqual([memory['id'] for memory in bridge.rank_memories(memories, 'hello', limit=2)], [4, 3])
        self.assertEqual(bridge.words('The Mill burned, wolf!'), {'mill', 'burned', 'wolf'})

    def test_database_reads_use_a_private_option_file_and_integer_parameters(self):
        database, calls = self.database()
        owners = database.owners()
        self.assertEqual([(row['owner_kind'], row['owner_id'], row['name']) for row in owners],
                         [('0', '1176', 'Humana'), ('1', '500', None)])
        memories = database.memories(0, 1176)
        self.assertEqual(memories[1]['claim'], 'the mill\tburned')
        self.assertEqual(memories[0]['reported_depth'], None)
        self.assertIn('WHERE owner_kind = 0 AND owner_id = 1176 ORDER BY salience DESC', calls[-1][1])
        for option_file, _ in calls:
            self.assertFalse(option_file.exists())
        with self.assertRaisesRegex(OSError, 'missing table'):
            database.query('SELECT 1 FROM nowhere')
        self.assertEqual(database.describe()['database'], 'acore_characters')
        self.assertNotIn(self.PASSWORD, json.dumps(database.describe()))

    def inspector(self, provider=None):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        path = Path(temp.name)
        (path / 'latest.json').write_text(json.dumps({
            'run': 'alles-1', 'seq': 1, 'source': 'alles-live', 'bots': [
                {'guid': 1176, 'name': 'Humana', 'level': 7, 'zone': 12, 'memoryCount': 3, 'pendingPerceptions': 2,
                 'memoryState': 'ready', 'memoryRevision': 50, 'committedRevision': 42, 'saving': False,
                 'saveFailed': False}]}))
        (path / 'manifest.json').write_text(json.dumps({'areas': [{'zone': 12, 'name': 'Elwynn Forest'}]}))
        database, calls = self.database()
        spool = bridge.Spool(path, 'x' * 32, follow=False)
        return bridge.MemoryInspector(database, provider, spool, maps=path, reasons={}), calls

    def test_overview_and_owner_merge_committed_rows_with_the_live_sample(self):
        inspector, _ = self.inspector()
        overview = inspector.overview()
        self.assertEqual(overview['talk'], None)
        self.assertEqual(overview['database']['database'], 'acore_characters')
        humana, creature = overview['owners']
        self.assertEqual((humana['owner'], humana['name'], humana['race'], humana['class'], humana['online']),
                         ('player:1176', 'Humana', 'Human', 'Priest', True))
        self.assertEqual((humana['committedMemories'], humana['committedPerceptions'], humana['committedRevision']),
                         (2, 1, 42))
        self.assertEqual(humana['live']['memoryRevision'], 50)
        self.assertEqual((creature['owner'], creature['name'], creature['live']), ('creature:500', 'creature:500', None))
        detail = inspector.owner('player:1176')
        self.assertEqual((detail['name'], detail['place'], detail['committedRevision']), ('Humana', 'Elwynn Forest', 42))
        self.assertEqual(detail['sheet']['class'], 'Priest')
        self.assertEqual(detail['live']['pendingPerceptions'], 2)
        wolf, mill = detail['memories']
        self.assertEqual(wolf['text'], 'I saw that Mangy Wolf died after being attacked by Humana')
        self.assertEqual((wolf['kind'], wolf['formation'], wolf['subject']['owner'], wolf['source']['owner']),
                         ('witnessed death', 'model', 'creature:80999', 'player:1176'))
        self.assertEqual((wolf['confidence'], wolf['salience'], wolf['formedUnixMs'], wolf['reportedDepth']),
                         (0.9, 0.75, 1788790000000, None))
        self.assertEqual((mill['text'], mill['reportedDepth'], mill['formation'], mill['subject']['owner']),
                         ('Humanb told me Humand reported that the mill\tburned', 2, 'fallback', None))
        self.assertEqual(detail['perceptions'][0], {
            'id': 3, 'kind': 'speech', 'subject': '', 'source': 'Humanb', 'comprehended': True, 'language': 7,
            'text': 'hello there', 'place': 'Northshire Valley', 'unixMs': 1788790500000, 'selfContext': 'standing'})
        with self.assertRaises(KeyError):
            inspector.owner('player:9')
        with self.assertRaises(ValueError):
            inspector.owner('player:x')
        unavailable = bridge.MemoryInspector(reasons={'database': 'no settings'})
        with self.assertRaisesRegex(bridge.Unavailable, 'no settings'):
            unavailable.overview()

    def test_talk_sends_a_bounded_context_and_answers_one_question_at_a_time(self):
        requests = []

        def opener(request, timeout):
            requests.append((request, timeout))
            return FakeResponse({'model': 'qwen3:8b-q8_0', 'choices': [{'finish_reason': 'stop', 'message': {
                'content': '  I remember a wolf.\nIt died.  '}}], 'usage': {'prompt_tokens': 300,
                                                                            'completion_tokens': 12}})

        provider = bridge.TalkProvider('http://gpu:11434/v1/', 'qwen3:8b-q8_0', timeout=7, opener=opener)
        inspector, _ = self.inspector(provider)
        answer = inspector.talk({'owner': 'player:1176', 'message': '  What about  the wolf? ',
                                 'history': [{'role': 'observer', 'text': 'hi'}, {'role': 'character', 'text': 'hello'}]})
        self.assertEqual(answer['text'], 'I remember a wolf. It died.')
        self.assertEqual((answer['name'], answer['message'], answer['promptTokens'], answer['completionTokens']),
                         ('Humana', 'What about the wolf?', 300, 12))
        self.assertEqual((answer['memoriesOffered'], answer['memoriesCommitted'], answer['committedRevision']),
                         ([7, 5], 2, 42))
        request, timeout = requests[0]
        self.assertEqual((request.full_url, timeout, request.get_method()), ('http://gpu:11434/v1/chat/completions', 7,
                                                                             'POST'))
        body = json.loads(request.data)
        self.assertEqual((body['model'], body['temperature'], body['reasoning_effort']), ('qwen3:8b-q8_0', 0.2, 'none'))
        self.assertEqual(body['messages'][0]['content'], bridge.TALK_CONTRACT)
        context = json.loads(body['messages'][1]['content'])
        self.assertEqual(context['character'], 'Humana, level 7 Human Priest')
        self.assertEqual(context['place'], 'Elwynn Forest')
        self.assertEqual(context['history'], ['Observer: hi', 'Humana: hello'])
        self.assertEqual(context['message'], 'What about the wolf?')
        self.assertTrue(context['memories'][0].startswith('I saw that Mangy Wolf died after being attacked by Humana '
                                                          '(confidence 0.90, salience 0.75, '))
        self.assertLessEqual(len(body['messages'][1]['content']) + len(bridge.TALK_CONTRACT), bridge.TALK_CONTEXT_BYTES)
        for bad in ({'owner': 'player:1176', 'message': ''}, {'owner': 'player:1176', 'message': 'x' * 501},
                    {'owner': 'player:1176', 'message': 'hi', 'history': [{'role': 'bot', 'text': 'x'}]},
                    {'owner': 'player:1176', 'message': 'hi', 'history': 'no'}, 'not an object'):
            with self.assertRaises(ValueError):
                inspector.talk(bad)
        with self.assertRaises(KeyError):
            inspector.talk({'owner': 'player:9', 'message': 'hi'})
        with provider.busy:
            with self.assertRaises(BlockingIOError):
                inspector.talk({'owner': 'player:1176', 'message': 'hi'})
        self.assertEqual(len(requests), 1)
        silent, _ = self.inspector()
        with self.assertRaisesRegex(bridge.Unavailable, 'worker'):
            silent.talk({'owner': 'player:1176', 'message': 'hi'})

    def test_talk_provider_failures_become_os_errors(self):
        def failing(request, timeout):
            raise bridge.urllib.error.URLError('connection refused')

        provider = bridge.TalkProvider('http://gpu:11434/v1', 'm', opener=failing)
        with self.assertRaisesRegex(OSError, 'unreachable'):
            provider.complete('s', 'u')
        empty = bridge.TalkProvider('http://gpu:11434/v1', 'm', opener=lambda *_, **__: FakeResponse({'choices': []}))
        with self.assertRaisesRegex(OSError, 'no completion'):
            empty.complete('s', 'u')
        blank = bridge.TalkProvider('http://gpu:11434/v1', 'm', opener=lambda *_, **__: FakeResponse(
            {'choices': [{'message': {'content': ' '}}]}))
        inspector, _ = self.inspector(blank)
        with self.assertRaisesRegex(OSError, 'empty reply'):
            inspector.talk({'owner': 'player:1176', 'message': 'hi'})

    def test_settings_come_from_the_world_and_worker_configuration(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            world = path / 'worldserver.conf'
            world.write_text('# comment\nCharacterDatabaseInfo = "127.0.0.1;3306;acore;secret;acore_characters"\n'
                             'Console.Enable = 1 # trailing\n')
            self.assertEqual(bridge.conf_value(world, 'CharacterDatabaseInfo'),
                             '127.0.0.1;3306;acore;secret;acore_characters')
            self.assertEqual(bridge.conf_value(world, 'Console.Enable'), '1')
            self.assertIsNone(bridge.conf_value(world, 'SOAP.Enabled'))
            self.assertIsNone(bridge.conf_value(path / 'missing.conf', 'X'))
            alles = path / 'alles.conf'
            alles.write_text(f'Alles.Worker.TokenFile = "{path / "setup" / "bridge-token"}"\n')
            self.assertIsNone(bridge.worker_config(None, alles))
            (path / 'setup').mkdir()
            (path / 'setup' / 'worker.json').write_text(json.dumps({'base_url': 'http://gpu:11434/v1', 'model': 'm',
                                                                   'bridge': '127.0.0.1:8779'}))
            self.assertEqual(bridge.worker_config(None, alles),
                             {'path': str(path / 'setup' / 'worker.json'), 'base_url': 'http://gpu:11434/v1',
                              'model': 'm'})
            (path / 'other.json').write_text(json.dumps({'model': 'm'}))
            self.assertIsNone(bridge.worker_config(path / 'other.json', alles))
            args = SimpleNamespace(no_memory=False, world_conf=world, alles_conf=alles, worker_config=None, mysql='mysql',
                                   maps=None)
            inspector = bridge.memory_inspector(args, None)
            self.assertEqual((inspector.database.database, inspector.provider.model, inspector.reasons), (
                'acore_characters', 'm', {}))
            args.world_conf = path / 'missing.conf'
            args.worker_config = path / 'nope.json'
            inspector = bridge.memory_inspector(args, None)
            self.assertIsNone(inspector.database)
            self.assertIsNone(inspector.provider)
            self.assertIn('No CharacterDatabaseInfo', inspector.reasons['database'])
            self.assertIn('nope.json', inspector.reasons['talk'])
            args.no_memory = True
            self.assertIn('--no-memory', bridge.memory_inspector(args, None).reasons['talk'])


if __name__ == '__main__':
    unittest.main()

class RaceAndQueueControls(unittest.TestCase):
    def setUp(self):
        Controls.setUp(self)
        self.snapshot.update(requestedSpeed=1, speedStep=0.1, backlogMs=0, seq=1, ready=True,
                             expectedBots=10, maxBots=50, observerMode=0, llmQueueLimit=8, llmGuard=False,
                             racePopulation=[{'race': race, 'capacity': 5, 'target': 5 if race in (1, 8) else 0}
                                             for race in (1, 2, 3, 4, 5, 6, 7, 8, 10, 11)])
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))

    def test_equal_total_race_swap_and_pending_quota_survive_speed_change(self):
        self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False,
                            'raceCounts': {'2': 5, '8': 5}, 'bots': 10})
        self.spool.control({'run': 'run-a', 'speed': 2, 'paused': False})
        fields = (self.path / 'control.txt').read_text().split()
        self.assertEqual(list(map(int, fields[8:])), [0, 5, 0, 0, 0, 0, 0, 5, 0, 0])

    def test_rejects_unprovisioned_race_capacity_and_mismatched_total(self):
        for extra in ({'raceCounts': {'8': 6}}, {'raceCounts': {'8': 5}, 'bots': 8},
                      {'raceCounts': {'9': 1}}, {'raceCounts': {'1': True}}):
            with self.assertRaises(ValueError):
                self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, **extra})

    def test_max_applies_queue_cap_and_numbered_speed_disables_guard(self):
        self.spool.control({'run': 'run-a', 'speed': 'max', 'paused': False, 'llmQueueLimit': 3})
        fields = (self.path / 'control.txt').read_text().split()
        self.assertEqual(fields[6:8], ['3', '1'])
        self.spool.control({'run': 'run-a', 'speed': 2, 'paused': False})
        fields = (self.path / 'control.txt').read_text().split()
        self.assertEqual(fields[6:8], ['3', '0'])
        for invalid in (0, 65, True, 1.5):
            with self.assertRaises(ValueError):
                self.spool.control({'run': 'run-a', 'speed': 'max', 'paused': False, 'llmQueueLimit': invalid})

    def test_max_backs_off_when_external_queue_holds_gameplay(self):
        controller = bridge.MaxSpeed('run-a', 100, 5, 0)
        state = dict(self.snapshot, requestedSpeed=4, llmQueueLimit=3, llmQueued=3, queueHeld=True)
        self.assertEqual(controller.choose(state, 1), 2)
        self.assertIn('LLM', controller.status)

class AutoPopulation(unittest.TestCase):
    def setUp(self):
        RaceAndQueueControls.setUp(self)

    def test_total_only_increase_defaults_to_humans(self):
        self.snapshot['racePopulation'][0]['capacity'] = 20
        self.snapshot['maxBots'] = 65
        (self.path / 'latest.json').write_text(json.dumps(self.snapshot))
        self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'bots': 12})
        fields = (self.path / 'control.txt').read_text().split()
        self.assertEqual(fields[4], '12')
        self.assertEqual(list(map(int, fields[8:])), [7, 0, 0, 0, 0, 0, 0, 5, 0, 0])

    def test_total_only_reduction_preserves_nonhuman_slots_first(self):
        self.spool.control({'run': 'run-a', 'speed': 1, 'paused': False, 'bots': 7})
        fields = (self.path / 'control.txt').read_text().split()
        self.assertEqual(list(map(int, fields[8:])), [2, 0, 0, 0, 0, 0, 0, 5, 0, 0])
