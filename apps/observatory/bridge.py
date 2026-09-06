#!/usr/bin/env python3
"""Local observatory HTTP/SSE adapter. Run beside the worldserver spool."""
import argparse
import hmac
import json
import math
import os
import re
from pathlib import Path
import secrets
import signal
from statistics import median
import sys
import threading
import time
from collections import Counter, deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

WEB = Path(__file__).with_name('web')
EXPORTS = {'manifest.json', 'initial.json', 'snapshots.ndjson', 'events.ndjson',
           'rollup.ndjson', 'events-rollup.ndjson', 'progression.ndjson', 'milestones.ndjson'}
SPEEDS = (1, 2, 5, 10)
DECIMAL_SPEEDS = tuple(tenths / 10 for tenths in range(10, 101))
OBSERVER_MODES = (0, 1, 2)
# Trace-only journal kinds (Observatory.Trace = 1). Keep in sync with TRACE_KINDS in web/model.js.
TRACE_KINDS = frozenset({
    'position', 'melee_swing', 'aura_tick', 'damage_input', 'damage', 'periodic_damage', 'cast_start',
    'cast_finish', 'cast_cancel', 'cooldown', 'regeneration', 'health_set', 'power_set', 'creature_death',
    'creature_respawn',
})
ACTIVITIES = ('idle', 'moving', 'combat', 'casting', 'dead')
# Two tiers per journal: the raw recent window (journal segments on disk, bounded deques in memory) and the
# long-term tier below, which folds every record into fixed simulated-time buckets kept for the whole run.
BUCKET_MS = 300000          # five simulated minutes per long-term point
MILESTONE_MS = 600000       # one complete snapshot retained every ten simulated minutes
LONG_TERM_POINTS = 20000    # long-term points held in memory per stream; the files keep every bucket
LONG_TERM_LIMIT = 720       # default points per long-term response; older buckets are folded together
JOURNALS = ('events', 'snapshots')
SEGMENT = re.compile(r'^(events|snapshots)\.(\d{6})\.ndjson$')


def load_token(path):
    """Return the bearer token, creating a private file (and parents) on first use."""
    path = Path(path)
    if not path.exists():
        try:
            path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            with os.fdopen(descriptor, 'w') as file:
                file.write(secrets.token_urlsafe(32))
    token = path.read_text().strip()
    if len(token) < 32:
        raise ValueError('Token must contain at least 32 characters')
    return token


def available_speeds(current):
    return DECIMAL_SPEEDS if current.get('speedStep') == 0.1 else SPEEDS


def valid_speed(speed):
    return (type(speed) in (int, float) and math.isfinite(speed) and 1 <= speed <= 10
            and math.isclose(speed * 10, round(speed * 10), rel_tol=0, abs_tol=1e-9))


def segments(directory, stem):
    """Rotated segments of a journal as (index, path), oldest first."""
    found = []
    try:
        for entry in os.scandir(directory):
            match = SEGMENT.match(entry.name)
            if match and match.group(1) == stem:
                found.append((int(match.group(2)), Path(entry.path)))
    except OSError:
        return []
    return sorted(found)


def journal_files(directory, stem):
    """Retained segments followed by the live journal, in record order."""
    return [path for _, path in segments(directory, stem)] + [Path(directory) / f'{stem}.ndjson']


def journal_tail(path, limit, byte_limit):
    """Read complete JSON objects from a bounded journal tail, tolerating an in-flight append."""
    try:
        with path.open('rb') as source:
            size = os.fstat(source.fileno()).st_size
            start = max(0, size - byte_limit)
            source.seek(start)
            data = source.read(byte_limit)
        lines = data.split(b'\n')[:-1]
        if start:
            lines = lines[1:]
        result = []
        for line in lines[-limit:]:
            try:
                value = json.loads(line)
                if isinstance(value, dict):
                    result.append(value)
            except ValueError:
                pass
        return result
    except OSError:
        return []


def journal_tail_files(paths, limit, byte_limit):
    """journal_tail across rotated segments: newest file first until the record or byte budget is spent."""
    result = []
    for path in reversed(paths):
        if len(result) >= limit or byte_limit <= 0:
            break
        try:
            size = path.stat().st_size
        except OSError:
            continue
        result = journal_tail(path, limit - len(result), byte_limit) + result
        byte_limit -= min(size, byte_limit)
    return result


def health_percent(bot):
    return 100 * (bot.get('health') or 0) / max(1, bot.get('maxHealth') or 1)


def summarize(frame):
    """One chart-ready long-term point per snapshot; mirrors summarize() in web/model.js."""
    bots = [bot for bot in frame.get('bots') or [] if isinstance(bot, dict)]
    levels = [bot.get('level') or 0 for bot in bots]
    point = {
        'bucket': frame['simMs'] // BUCKET_MS * BUCKET_MS, 'samples': 1, 'run': frame.get('run'),
        'simMs': frame['simMs'], 'realMs': frame.get('realMs'), 'seq': frame.get('seq'),
        'xp': sum(bot.get('earnedXp') or 0 for bot in bots),
        'quests': sum(bot.get('questCompletions') or 0 for bot in bots),
        'deaths': sum(bot.get('deaths') or 0 for bot in bots),
        'levels': dict(Counter(f'Level {level}' for level in levels)),
        'zones': dict(Counter(f"{bot.get('map')}/{bot.get('zone')}" for bot in bots)),
        'activity': {activity: 0 for activity in ACTIVITIES},
        'inWorld': len(bots), 'online': frame.get('onlineBots'), 'active': frame.get('activeBots'),
        'expected': frame.get('expectedBots'), 'requestedSpeed': frame.get('requestedSpeed'),
        'achievedSpeed': frame.get('achievedSpeed'), 'backlogMs': frame.get('backlogMs'),
        'tickMs': None if frame.get('maxTickUs') is None else frame['maxTickUs'] / 1000,
        'pausedShare': 1 if frame.get('paused') else 0,
        'meanHealth': sum(map(health_percent, bots)) / len(bots) if bots else None,
        'money': (sum(bot.get('money') or 0 for bot in bots)
                  if any(bot.get('money') is not None for bot in bots) else None),
        'questsActive': sum(len(bot.get('quests') or []) for bot in bots),
        'meanLevel': sum(levels) / len(levels) if levels else None,
        'minLevel': min(levels) if levels else None, 'maxLevel': max(levels) if levels else None,
    }
    for bot in bots:
        activity = bot.get('activity')
        point['activity'][activity if activity in ACTIVITIES else 'idle'] += 1
    if isinstance(frame.get('runTotals'), dict):
        point.update(frame['runTotals'])
    return point


MEAN_KEYS = ('meanLevel', 'meanHealth', 'achievedSpeed', 'requestedSpeed', 'backlogMs', 'inWorld', 'online',
             'active', 'expected', 'pausedShare')


def weighted(a, b, weight_a, weight_b):
    if a is None or b is None:
        return b if a is None else a
    return (a * weight_a + b * weight_b) / (weight_a + weight_b)


def extreme(choose, *values):
    known = [value for value in values if value is not None]
    return choose(known) if known else None


def ordered(a, b):
    return (a, b) if (b.get('seq') or 0) >= (a.get('seq') or 0) else (b, a)


def bucket_span(a, b):
    """Distinct buckets folded into a point; records of one bucket accumulate without widening it."""
    if a['bucket'] == b['bucket']:
        return max(a.get('buckets', 1), b.get('buckets', 1))
    return a.get('buckets', 1) + b.get('buckets', 1)


def combine_points(a, b):
    """Merge two snapshot buckets: counters and distributions keep the newest value, gauges become
    sample-weighted means, levels keep their extremes and the longest tick survives."""
    older, newer = ordered(a, b)
    merged = dict(older)
    merged.update(newer)
    merged['bucket'] = min(a['bucket'], b['bucket'])
    merged['buckets'] = bucket_span(a, b)
    merged['samples'] = a['samples'] + b['samples']
    for key in MEAN_KEYS:
        merged[key] = weighted(a.get(key), b.get(key), a['samples'], b['samples'])
    merged['activity'] = {
        activity: weighted((a.get('activity') or {}).get(activity), (b.get('activity') or {}).get(activity),
                           a['samples'], b['samples'])
        for activity in ACTIVITIES}
    merged['tickMs'] = extreme(max, a.get('tickMs'), b.get('tickMs'))
    merged['minLevel'] = extreme(min, a.get('minLevel'), b.get('minLevel'))
    merged['maxLevel'] = extreme(max, a.get('maxLevel'), b.get('maxLevel'))
    return merged


def combine_counts(a, b):
    """Merge two event-count buckets by adding their per-kind counts."""
    older, newer = ordered(a, b)
    merged = dict(older)
    merged.update(newer)
    merged['bucket'] = min(a['bucket'], b['bucket'])
    merged['buckets'] = bucket_span(a, b)
    merged['samples'] = a['samples'] + b['samples']
    merged['kinds'] = dict(Counter(a['kinds']) + Counter(b['kinds']))
    return merged


def coarsen(points, limit, combine):
    """Fold consecutive buckets together so at most `limit` points are returned."""
    if len(points) <= limit:
        return list(points)
    size = math.ceil(len(points) / limit)
    result = []
    for start in range(0, len(points), size):
        merged = points[start]
        for point in points[start + 1:start + size]:
            merged = combine(merged, point)
        result.append(merged)
    return result


class Rollup:
    """Long-term tier of one stream: fixed simulated-time buckets combined as records arrive, appended to
    an NDJSON file when they close, and folded on read so a run of any length fits one bounded response."""

    def __init__(self, path, combine, limit=LONG_TERM_POINTS):
        self.path = Path(path)
        self.combine = combine
        self.points = deque(maxlen=limit)
        self.current = None
        self.stored = 0
        self.last_seq = -1
        # A bucket flushed at shutdown and completed after a restart is stored twice; merge such neighbours.
        for point in self._stored():
            if self.points and self.points[-1]['bucket'] == point['bucket']:
                self.points[-1] = combine(self.points[-1], point)
            else:
                self.points.append(point)
                self.stored += 1
            self.last_seq = max(self.last_seq, point.get('seq') or -1)

    def _stored(self):
        try:
            with self.path.open('rb') as source:
                for line in source:
                    try:
                        value = json.loads(line)
                    except ValueError:
                        continue
                    if isinstance(value, dict) and type(value.get('bucket')) is int and 'samples' in value:
                        yield value
        except OSError:
            return

    def add(self, point):
        # A record stamped before the open bucket folds into it rather than reopening a closed one.
        if self.current and point['bucket'] > self.current['bucket']:
            self.close()
        self.current = self.combine(self.current, point) if self.current else point
        self.last_seq = max(self.last_seq, point.get('seq') or -1)

    def close(self):
        if not self.current:
            return
        with self.path.open('a') as target:
            target.write(json.dumps(self.current) + '\n')
        self.points.append(self.current)
        self.stored += 1
        self.current = None

    def snapshot(self, limit=LONG_TERM_LIMIT):
        points = list(self.points)
        if self.current:
            points.append(dict(self.current, partial=True))
        return coarsen(points, limit, self.combine)


class JournalFollower:
    """Follow an append-only journal by holding its descriptor. When the world rotates the file into a numbered
    segment, the old descriptor is drained to its end before the successor is read from its beginning, so no
    record is skipped or double counted. Only the first open seeds from the existing tail."""

    def __init__(self, path, seed_bytes=262144, start_at_end=True):
        self.path = Path(path)
        self.lock = threading.Lock()
        self.file = None
        self.identity = None
        self.partial = b''
        self.seed_bytes = seed_bytes
        self.start_at_end = start_at_end
        self.opened = False

    def handle(self, records, counted):
        raise NotImplementedError

    def close(self):
        if self.file:
            self.file.close()
            self.file = None

    def poll(self):
        if self.file:
            self.drain()
        try:
            stat = self.path.stat()
        except OSError:
            return
        if (stat.st_dev, stat.st_ino) == self.identity:
            return
        if self.file:
            # The world closes a segment before renaming it, so the old descriptor is final: drain it.
            self.drain()
            self.file.close()
            self.file = None
        try:
            self.file = self.path.open('rb')
            stat = os.fstat(self.file.fileno())
        except OSError:
            self.file = None
            self.identity = None
            return
        self.identity = (stat.st_dev, stat.st_ino)
        self.partial = b''
        seeding = not self.opened and self.start_at_end
        self.opened = True
        if seeding and stat.st_size > self.seed_bytes:
            self.file.seek(stat.st_size - self.seed_bytes)
            self.file.readline()
        self.drain(counted=not seeding)

    def drain(self, counted=True):
        """Read every appended byte; the seed batch restores display state but is not counted."""
        while self.file:
            try:
                if os.fstat(self.file.fileno()).st_size < self.file.tell():
                    raise OSError('journal truncated')
                data = self.file.read(4 * 1024 * 1024)
            except OSError:
                self.file.close()
                self.file = None
                self.identity = None
                return
            if not data:
                return
            lines = (self.partial + data).split(b'\n')
            self.partial = lines.pop()
            records = []
            for line in lines:
                try:
                    value = json.loads(line)
                except ValueError:
                    continue
                if isinstance(value, dict):
                    records.append(value)
            self.handle(records, counted)


def simulated_ms(record):
    value = record.get('simMs')
    return value if type(value) in (int, float) and math.isfinite(value) else None


class EventTail(JournalFollower):
    """Recent tier: the newest records and per-kind counts. Long-term tier: per-kind counts per simulated
    bucket in events-rollup.ndjson and every non-trace record in progression.ndjson.

    With tracing enabled the journal is dominated by per-step trace records, so the bounded file tail alone
    hides progression. The follower reads only newly appended bytes and never rescans the file.
    """

    def __init__(self, path, recent=500, progression=2000, start_at_end=True, long_term=True):
        super().__init__(path, start_at_end=start_at_end)
        self.recent = deque(maxlen=recent)
        self.progression = deque(maxlen=progression)
        self.kinds = Counter()
        self.minute = deque()
        self.since = int(time.time() * 1000)
        self.rates = self.progression_file = None
        self.progression_seq = -1
        if long_term:
            self.rates = Rollup(self.path.with_name('events-rollup.ndjson'), combine_counts)
            self.progression_file = self.path.with_name('progression.ndjson')
            last = journal_tail(self.progression_file, 1, 65536)
            self.progression_seq = (last[-1].get('seq') if last else None) or -1

    def handle(self, records, counted):
        now = time.time()
        buckets = {}
        kept = []
        with self.lock:
            for value in records:
                if not isinstance(value.get('kind'), str):
                    continue
                self.recent.append(value)
                if counted:
                    self.kinds[value['kind']] += 1
                    self.minute.append((now, value['kind']))
                    stamp = simulated_ms(value)
                    if self.rates and stamp is not None:
                        bucket = buckets.setdefault(int(stamp // BUCKET_MS * BUCKET_MS), {'kinds': Counter()})
                        bucket['kinds'][value['kind']] += 1
                        bucket['simMs'], bucket['seq'] = stamp, value.get('seq')
                if value['kind'] not in TRACE_KINDS:
                    self.progression.append(value)
                    if (self.progression_file and type(value.get('seq')) is int
                            and value['seq'] > self.progression_seq):
                        kept.append(value)
                        self.progression_seq = value['seq']
            while self.minute and now - self.minute[0][0] > 60:
                self.minute.popleft()
            for start in sorted(buckets):
                bucket = buckets[start]
                self.rates.add({'bucket': start, 'simMs': bucket['simMs'], 'seq': bucket['seq'],
                                'samples': sum(bucket['kinds'].values()), 'kinds': dict(bucket['kinds'])})
            if kept:
                with self.progression_file.open('a') as target:
                    target.writelines(json.dumps(value) + '\n' for value in kept)

    def events(self, scope):
        with self.lock:
            return list(self.progression if scope == 'progression' else self.recent)

    def stats(self):
        with self.lock:
            cutoff = time.time() - 60
            recent = Counter(kind for stamp, kind in self.minute if stamp >= cutoff)
            return {'since': self.since, 'total': sum(self.kinds.values()), 'kinds': dict(self.kinds),
                    'recent': dict(recent)}

    def long_term(self, limit=LONG_TERM_LIMIT):
        with self.lock:
            return {'bucketMs': BUCKET_MS, 'points': self.rates.snapshot(limit) if self.rates else []}

    def close(self):
        super().close()
        with self.lock:
            if self.rates:
                self.rates.close()


class SnapshotTail(JournalFollower):
    """Long-term snapshot tier: summary buckets in rollup.ndjson plus complete milestone frames in
    milestones.ndjson. Seed frames are real history and enter the rollup unless it already holds them; a bridge
    without a rollup yet backfills it from the whole retained snapshot journal instead of only its tail."""

    def __init__(self, path, seed_bytes=4 * 1024 * 1024):
        super().__init__(path, seed_bytes=seed_bytes)
        self.rollup = Rollup(self.path.with_name('rollup.ndjson'), combine_points)
        self.start_at_end = self.rollup.stored > 0
        self.milestones = self.path.with_name('milestones.ndjson')
        self.milestone_count = 0
        self.milestone_slot = -1
        for frame in journal_tail(self.milestones, 1, 4 * 1024 * 1024):
            stamp = simulated_ms(frame)
            if stamp is not None:
                self.milestone_slot = int(stamp // MILESTONE_MS)
        try:
            with self.milestones.open('rb') as source:
                self.milestone_count = sum(1 for _ in source)
        except OSError:
            pass

    def handle(self, records, counted):
        with self.lock:
            for frame in records:
                if (type(frame.get('simMs')) is not int or type(frame.get('seq')) is not int
                        or not isinstance(frame.get('bots'), list) or frame['seq'] <= self.rollup.last_seq):
                    continue
                self.rollup.add(summarize(frame))
                slot = frame['simMs'] // MILESTONE_MS
                if slot > self.milestone_slot:
                    with self.milestones.open('a') as target:
                        target.write(json.dumps(frame) + '\n')
                    self.milestone_slot = slot
                    self.milestone_count += 1

    def long_term(self, limit=LONG_TERM_LIMIT):
        with self.lock:
            return {'bucketMs': BUCKET_MS, 'points': self.rollup.snapshot(limit)}

    def close(self):
        super().close()
        with self.lock:
            self.rollup.close()


class MaxSpeed:
    """Probe available rates; throttle sustained accumulation rather than individual backlog spikes."""

    def __init__(self, run, limit, sequence, now):
        self.run = run
        self.limit = limit
        self.sequence = sequence
        self.frame = None
        self.fresh_at = now
        self.stable_since = None
        self.samples = deque(maxlen=64)
        self.status = 'Waiting for world acknowledgement'

    def reset_measurement(self):
        self.samples.clear()
        self.stable_since = None

    def choose(self, current, now):
        if self.frame and current['seq'] == self.frame['seq']:
            if now - self.fresh_at >= 3:
                self.status = 'Waiting for fresh telemetry'
                self.reset_measurement()
            return None
        previous, elapsed = self.frame, now - self.fresh_at
        self.frame, self.fresh_at = current, now
        if elapsed >= 3 or (previous and previous['requestedSpeed'] != current['requestedSpeed']):
            self.reset_measurement()
        if current['controlSeq'] < self.sequence:
            self.status = 'Waiting for world acknowledgement'
            self.reset_measurement()
            return None
        if current.get('paused'):
            self.status = 'Paused'
            self.reset_measurement()
            return None
        if not current.get('ready') or current.get('populationPending'):
            self.status = 'Waiting for the bot population'
            self.reset_measurement()
            return 1 if current['requestedSpeed'] > 1 else None

        speed, backlog = current['requestedSpeed'], current['backlogMs']
        speeds = available_speeds(current)
        self.samples.append((now, backlog))
        while len(self.samples) > 1 and self.samples[1][0] <= now - 3:
            self.samples.popleft()
        if self.stable_since is None:
            self.stable_since = now

        # Compare three one-second medians. A single spike, a plateau, or clearing debt is not
        # sustained growth. Only fresh samples at the acknowledged speed enter this window.
        buckets = ([value for stamp, value in self.samples if stamp <= now - 2],
                   [value for stamp, value in self.samples if now - 2 < stamp <= now - 1],
                   [value for stamp, value in self.samples if stamp > now - 1])
        if now - self.samples[0][0] < 3 or not all(buckets):
            self.status = 'Measuring backlog trend'
            return None
        early, middle, recent = map(median, buckets)
        noise = max(1, self.limit * 0.01)
        growing = middle > early + noise and recent > middle + noise and backlog >= recent
        if growing:
            self.stable_since = None
            if recent < self.limit:
                self.status = 'Watching growing backlog'
                return None
            if speed == 1:
                self.status = 'At 1×; backlog is still accumulating'
                return None
            self.status = 'Reducing speed: backlog has been accumulating for 3 seconds'
            if current.get('speedStep') == 0.1:
                # Debt growth is requested rate minus capacity. Leave 0.1x headroom to drain it;
                # a large capacity loss must not take dozens of 0.1x reductions to recover.
                span = (median(stamp for stamp, _ in self.samples if stamp > now - 1)
                        - median(stamp for stamp, _ in self.samples if stamp <= now - 2))
                capacity = speed - (recent - early) / span / 1000
                reduced = max(1, min(round(speed - 0.1, 1), round(math.floor(capacity * 10) / 10 - 0.1, 1)))
                self.reset_measurement()
                return reduced
            self.reset_measurement()
            return speeds[speeds.index(speed) - 1]
        if recent > self.limit and recent < early - noise:
            self.stable_since = None
            self.status = 'Holding speed while backlog drains'
            return None
        if speed == speeds[-1]:
            self.status = 'Running at the highest available speed'
        elif now - self.stable_since >= 5:
            self.reset_measurement()
            self.status = 'Trying the next speed'
            return speeds[speeds.index(speed) + 1]
        else:
            self.status = 'Measuring sustainable speed'
        return None


class Spool:
    def __init__(self, directory, token, follow=True, retain_bytes=2 * 1024 ** 3):
        self.directory = Path(directory)
        self.token = token
        self.lock = threading.RLock()
        self.sequence = 0
        self.max_speed = None
        self.backlog_limit_ms = 100
        self.speed_status = 'Manual speed'
        self.viewers = threading.BoundedSemaphore(16)
        self.retain_bytes = retain_bytes
        self.tail = EventTail(self.directory / 'events.ndjson')
        self.snapshots = SnapshotTail(self.directory / 'snapshots.ndjson')
        self.pruned = {stem: {'segments': 0, 'bytes': 0} for stem in JOURNALS}
        self.rotated = False
        self.stop = threading.Event()
        if follow:
            threading.Thread(target=self.follow_journals, daemon=True).start()
            threading.Thread(target=self.follow_speed, daemon=True).start()

    def close(self):
        self.stop.set()
        self.tail.close()
        self.snapshots.close()

    def follow_journals(self, interval=0.25):
        pruned_at = time.monotonic()
        while not self.stop.wait(interval):
            self.tail.poll()
            self.snapshots.poll()
            if time.monotonic() - pruned_at >= 30:
                pruned_at = time.monotonic()
                self.prune()

    def prune(self):
        """Delete the oldest consumed segments of each journal while the retained window exceeds the budget.
        The live file is never touched: without world-side rotation nothing is deleted."""
        for stem, follower in (('events', self.tail), ('snapshots', self.snapshots)):
            found = []
            for _, path in segments(self.directory, stem):
                try:
                    stat = path.stat()
                except OSError:
                    continue
                found.append((path, stat.st_size, (stat.st_dev, stat.st_ino)))
            self.rotated = self.rotated or bool(found)
            total = sum(size for _, size, _ in found)
            for path, size, identity in found:
                # A segment the follower still holds, and everything after it, is not yet folded in.
                if total <= self.retain_bytes or identity == follower.identity:
                    break
                try:
                    path.unlink()
                except OSError:
                    break
                total -= size
                self.pruned[stem]['segments'] += 1
                self.pruned[stem]['bytes'] += size

    def retention(self):
        def size(path):
            try:
                return path.stat().st_size
            except OSError:
                return 0

        journals = {}
        for stem in JOURNALS:
            found = segments(self.directory, stem)
            journals[stem] = {'currentBytes': size(self.directory / f'{stem}.ndjson'), 'segments': len(found),
                              'segmentBytes': sum(size(path) for _, path in found),
                              'prunedSegments': self.pruned[stem]['segments'],
                              'prunedBytes': self.pruned[stem]['bytes']}
        rotation = self.rotated or any(journal['segments'] for journal in journals.values())
        try:
            settings = json.loads((self.directory / 'manifest.json').read_text()).get('settings') or {}
            rotation = rotation or int(settings.get('Observatory.JournalSegmentBytes') or 0) > 0
        except (OSError, ValueError, TypeError, AttributeError):
            pass
        return {'retainBytes': self.retain_bytes, 'bucketMs': BUCKET_MS, 'milestoneMs': MILESTONE_MS,
                'rotation': rotation, 'journals': journals,
                'longTerm': {'snapshotBuckets': self.snapshots.rollup.stored,
                             'eventBuckets': self.tail.rates.stored if self.tail.rates else 0,
                             'milestones': self.snapshots.milestone_count,
                             'progressionBytes': size(self.directory / 'progression.ndjson')}}

    def snapshot(self):
        with self.lock:
            current = json.loads((self.directory / 'latest.json').read_text())
            current['speedControl'] = {
                'mode': 'max' if self.max_speed else 'manual',
                'backlogLimitMs': self.backlog_limit_ms,
                'status': self.max_speed.status if self.max_speed else self.speed_status,
            }
            return current

    def follow_speed(self):
        while not self.stop.wait(0.25):
            try:
                self.adjust_speed()
            except (OSError, ValueError, KeyError, TypeError):
                with self.lock:
                    if self.max_speed:
                        self.max_speed.status = 'Waiting for valid telemetry and control mailbox'
                        self.max_speed.reset_measurement()

    def adjust_speed(self, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            controller = self.max_speed
            if not controller:
                return
            current = self.snapshot()
            if (current['run'] != controller.run or current.get('readOnly') or current.get('source') == 'python-api'
                    or current.get('baseline') or current.get('completed') or current.get('fault')
                    or current.get('observers') or current['controlSeq'] > controller.sequence
                    or (current['controlSeq'] == controller.sequence and current.get('controlError'))):
                self.max_speed = None
                self.speed_status = 'Max stopped: run state or another controller changed'
                return
            fields = (self.directory / 'control.txt').read_text().split()
            if fields[:2] != [controller.run, str(controller.sequence)]:
                self.max_speed = None
                self.speed_status = 'Max stopped: another controller changed speed'
                return
            self.validate_speed_telemetry(current)
            speed = controller.choose(current, now)
            if speed is not None:
                result = self._control({'run': current['run'], 'speed': speed, 'paused': False})
                controller.sequence = result['sequence']

    @staticmethod
    def validate_speed_telemetry(current):
        if (not valid_speed(current.get('requestedSpeed'))
                or current['requestedSpeed'] not in available_speeds(current) or type(current.get('seq')) is not int
                or type(current.get('backlogMs')) not in (int, float)
                or not math.isfinite(current['backlogMs']) or current['backlogMs'] < 0):
            raise ValueError('Max requires valid speed and backlog telemetry')

    def control(self, request):
        required = {'run', 'speed', 'paused'}
        if (not required <= set(request) or set(request) - required - {'bots', 'backlogLimitMs', 'observerMode'}
                or not (request['speed'] == 'max'
                        or valid_speed(request['speed']))
                or type(request['paused']) is not bool):
            raise ValueError('Expected run, speed (1–10 in 0.1 steps, or max), and paused (boolean)')
        if 'backlogLimitMs' in request and (request['speed'] != 'max'
                or type(request['backlogLimitMs']) is not int or not 10 <= request['backlogLimitMs'] <= 60000):
            raise ValueError('backlogLimitMs requires Max and must be an integer from 10 to 60000')
        if 'bots' in request and (type(request['bots']) is not int or not 0 <= request['bots'] <= 100):
            raise ValueError('bots must be an integer from 0 to 100')
        if 'observerMode' in request and (type(request['observerMode']) is not int
                                         or request['observerMode'] not in OBSERVER_MODES):
            raise ValueError('observerMode must be 0 (locked), 1 (roam) or 2 (full GM)')
        with self.lock:
            numeric = dict(request)
            numeric.pop('backlogLimitMs', None)
            if request['speed'] != 'max':
                numeric['speed'] = round(request['speed'], 1)
            if request['speed'] == 'max':
                current = self.snapshot()
                if current.get('baseline') or current.get('observers'):
                    raise ValueError('Max is unavailable during real-time baseline or GM POV')
                self.validate_speed_telemetry(current)
                numeric['speed'] = current['requestedSpeed'] if self.max_speed else 1
            result = self._control(numeric)
            if request['speed'] == 'max':
                self.backlog_limit_ms = request.get('backlogLimitMs', self.backlog_limit_ms)
                self.max_speed = MaxSpeed(request['run'], self.backlog_limit_ms,
                                          result['sequence'], time.monotonic())
            else:
                self.max_speed = None
                self.speed_status = 'Manual speed'
            return result

    def _control(self, request):
        with self.lock:
            current = self.snapshot()
            if current.get('readOnly') or current.get('source') == 'python-api':
                raise ValueError('This observation feed is read-only; use the Python controller for actions')
            if request['run'] != current['run']:
                raise ValueError('Run changed; reconnect before controlling')
            if request['speed'] not in available_speeds(current):
                raise ValueError('Update the worldserver before requesting speeds outside 1, 2, 5 and 10')
            if current.get('baseline') and (request['speed'] != 1 or request['paused']):
                raise ValueError('Real-time baseline supports 1x without pause only')
            if current.get('observers', 0) and (request['speed'] != 1 or request['paused']):
                raise ValueError('GM POV requires 1x without pause until all observers disconnect')
            if current.get('completed'):
                raise ValueError('The configured simulated duration has completed')
            if current['fault']:
                raise ValueError('Run is invalid; inspect server and start a fresh run')
            bots = current.get('expectedBots', 100)
            observer_mode = current.get('observerMode', 0)
            try:
                fields = (self.directory / 'control.txt').read_text().split()
                run, sequence = fields[:2]
                if (run == current['run'] and len(fields) >= 5
                        and (int(sequence) > current['controlSeq'] or not current.get('controlError'))):
                    bots = int(fields[4])
                    if len(fields) >= 6:
                        observer_mode = int(fields[5])
                if run == current['run']:
                    self.sequence = max(self.sequence, int(sequence))
            except (OSError, ValueError):
                pass
            bots = request.get('bots', bots)
            if 'bots' in request and 'maxBots' not in current:
                raise ValueError('Update the worldserver before controlling population')
            if bots > current.get('maxBots', 100):
                raise ValueError('Target exceeds the configured bot pool')
            if current.get('baseline') and bots != current.get('expectedBots'):
                raise ValueError('Real-time baseline has a fixed population')
            if 'observerMode' in request and 'observerMode' not in current:
                raise ValueError('Update the worldserver before controlling GM observer mode')
            observer_mode = request.get('observerMode', observer_mode)
            self.sequence = max(self.sequence, current['controlSeq']) + 1
            # The world acknowledges application in subsequent snapshots; HTTP 202 is acceptance only.
            text = f"{request['run']} {self.sequence} {request['speed']:g} {int(request['paused'])}"
            if 'maxBots' in current:
                text += f" {bots}"
            if 'observerMode' in current:
                text += f" {observer_mode}"
            text += "\n"
            temporary = self.directory / 'control.txt.tmp'
            temporary.write_text(text)
            os.replace(temporary, self.directory / 'control.txt')
            return {'run': request['run'], 'sequence': self.sequence, 'status': 'accepted'}


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        pass

    def reply(self, status, payload, content_type='application/json'):
        if not isinstance(payload, bytes):
            payload = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(payload)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Security-Policy', "default-src 'self'; style-src 'self'; object-src 'none'")
        self.end_headers()
        self.wfile.write(payload)

    def authorized(self):
        return hmac.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + self.server.spool.token)

    def do_GET(self):
        path = urlparse(self.path).path
        static = {'/': ('index.html', 'text/html'), '/app.js': ('app.js', 'text/javascript'),
                  '/model.js': ('model.js', 'text/javascript'), '/charts.js': ('charts.js', 'text/javascript'),
                  '/style.css': ('style.css', 'text/css')}
        if path in static:
            name, mime = static[path]
            self.reply(200, (WEB / name).read_bytes(), mime)
            return
        if path == '/favicon.ico':
            self.reply(204, b'', 'image/x-icon')
            return
        if not self.authorized():
            self.reply(401, {'error': 'Bearer token required'})
            return
        spool = self.server.spool
        maps = getattr(self.server, 'maps', None)
        if path == '/api/maps':
            try:
                self.reply(200, json.loads((maps / 'manifest.json').read_text()) if maps else {'areas': []})
            except (OSError, ValueError):
                self.reply(503, {'error': 'Map artwork is unavailable'})
        elif re.fullmatch(r'/api/maps/\d+-\d+(?:-\d+)?\.png', path):
            try:
                if not maps:
                    raise FileNotFoundError()
                self.reply(200, (maps / path.rsplit('/', 1)[-1]).read_bytes(), 'image/png')
            except OSError:
                self.reply(404, {'error': 'Map tile unavailable'})
        elif path == '/api/stream':
            self.stream()
        elif path == '/api/snapshot':
            try:
                self.reply(200, spool.snapshot())
            except (OSError, ValueError):
                self.reply(503, {'error': 'Waiting for worldserver telemetry'})
        elif path.startswith('/api/export/') and path.rsplit('/', 1)[-1] in EXPORTS:
            self.export(path.rsplit('/', 1)[-1])
        elif path == '/api/events':
            if self.scope() == 'progression':
                self.reply(200, spool.tail.events('progression'))
            else:
                self.reply(200, journal_tail_files(journal_files(spool.directory, 'events'), 500, 262144))
        elif path == '/api/event-stats':
            if self.scope() == 'long-term':
                self.reply(200, spool.tail.long_term(self.limit()))
            else:
                self.reply(200, spool.tail.stats())
        elif path == '/api/history':
            if self.scope() == 'long-term':
                self.reply(200, spool.snapshots.long_term(self.limit()))
            else:
                self.reply(200, journal_tail_files(journal_files(spool.directory, 'snapshots'), 1000,
                                                   4 * 1024 * 1024))
        elif path == '/api/retention':
            self.reply(200, spool.retention())
        else:
            self.reply(404, {'error': 'Not found'})

    def scope(self):
        return parse_qs(urlparse(self.path).query).get('scope', [''])[0]

    def limit(self):
        try:
            return max(1, min(5000, int(parse_qs(urlparse(self.path).query).get('limit', [''])[0])))
        except ValueError:
            return LONG_TERM_LIMIT

    def export(self, name):
        """Stream one file, or a rotated journal's retained segments followed by its live file. Every part is
        opened first: the fixed content length is an export cutoff even while the journal grows or is pruned."""
        directory = self.server.spool.directory
        stem = name[:-len('.ndjson')]
        paths = journal_files(directory, stem) if stem in JOURNALS else [directory / name]
        sources = []
        try:
            for path in paths:
                try:
                    sources.append(path.open('rb'))
                except FileNotFoundError:
                    if path.name == name:
                        self.reply(404, {'error': f'{name} is not available yet'})
                        return
            parts = [(source, os.fstat(source.fileno()).st_size) for source in sources]
            self.send_response(200)
            self.send_header('Content-Type', 'application/x-ndjson' if name.endswith('.ndjson')
                             else 'application/json')
            self.send_header('Content-Length', str(sum(size for _, size in parts)))
            self.send_header('Content-Disposition', f'attachment; filename="{name}"')
            self.end_headers()
            for source, remaining in parts:
                while remaining:
                    chunk = source.read(min(65536, remaining))
                    if not chunk:
                        raise OSError('export source shrank')
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
        except OSError:
            self.close_connection = True
        finally:
            for source in sources:
                source.close()

    def stream(self):
        spool = self.server.spool
        if not spool.viewers.acquire(blocking=False):
            self.reply(503, {'error': 'Viewer limit reached'})
            return
        self.connection.settimeout(5)
        try:
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Connection', 'close')
            self.end_headers()
            previous = None
            while True:
                try:
                    data = spool.snapshot()
                    identity = (data['run'], data['seq'])
                    if identity != previous:
                        self.wfile.write(f'data: {json.dumps(data)}\n\n'.encode())
                        previous = identity
                    else:
                        self.wfile.write(b': heartbeat\n\n')
                except (OSError, ValueError):
                    self.wfile.write(b': waiting for telemetry\n\n')
                self.wfile.flush()
                time.sleep(0.25)
        except (OSError, TimeoutError):
            pass
        finally:
            spool.viewers.release()
            self.close_connection = True

    def do_POST(self):
        if not self.authorized():
            self.reply(401, {'error': 'Bearer token required'})
            return
        # A local bearer token plus JSON and no CORS prevents cross-origin form control requests.
        if self.path != '/api/control' or self.headers.get('Content-Type') != 'application/json':
            self.reply(415, {'error': 'Use POST /api/control with application/json'})
            return
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if size <= 0 or size > 1024:
                raise ValueError('Invalid request size')
            self.connection.settimeout(5)
            request = json.loads(self.rfile.read(size))
            if not isinstance(request, dict):
                raise ValueError('Expected an object')
            result = self.server.spool.control(request)
            self.reply(202, result)
        except (ValueError, KeyError, TypeError) as error:
            self.reply(400, {'error': str(error)})
        except OSError:
            self.reply(503, {'error': 'Worldserver spool unavailable'})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spool', required=True)
    parser.add_argument('--port', type=int, default=8787)
    parser.add_argument('--token-file', type=Path, required=True)
    parser.add_argument('--maps', type=Path, help='Local artwork directory created by extract_maps.py')
    parser.add_argument('--retain-bytes', type=int, default=2 * 1024 ** 3,
                        help='Rotated journal segments kept per journal before the oldest are deleted '
                             '(default 2 GiB; requires Observatory.JournalSegmentBytes in the world)')
    args = parser.parse_args()
    try:
        token = load_token(args.token_file)
    except OSError as error:
        parser.error(f'Cannot create or read token file {args.token_file}: {error}')
    except ValueError as error:
        parser.error(str(error))
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.daemon_threads = True
    server.spool = Spool(args.spool, token, retain_bytes=max(0, args.retain_bytes))
    server.maps = args.maps
    print(f'Observatory: http://127.0.0.1:{args.port}; token: {args.token_file}')
    # A service stop must flush the open long-term buckets, so SIGTERM ends serve_forever through SystemExit.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        server.serve_forever()
    finally:
        server.spool.close()


if __name__ == '__main__':
    main()
