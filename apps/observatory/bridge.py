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
from statistics import median
import threading
import time
from collections import Counter, deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

WEB = Path(__file__).with_name('web')
EXPORTS = {'manifest.json', 'initial.json', 'snapshots.ndjson', 'events.ndjson'}
SPEEDS = (1, 2, 5, 10)
DECIMAL_SPEEDS = tuple(tenths / 10 for tenths in range(10, 101))
OBSERVER_MODES = (0, 1, 2)
# Trace-only journal kinds (Observatory.Trace = 1). Keep in sync with TRACE_KINDS in web/model.js.
TRACE_KINDS = frozenset({
    'position', 'melee_swing', 'aura_tick', 'damage_input', 'damage', 'periodic_damage', 'cast_start',
    'cast_finish', 'cast_cancel', 'cooldown', 'regeneration', 'health_set', 'power_set', 'creature_death',
    'creature_respawn',
})


def available_speeds(current):
    return DECIMAL_SPEEDS if current.get('speedStep') == 0.1 else SPEEDS


def valid_speed(speed):
    return (type(speed) in (int, float) and math.isfinite(speed) and 1 <= speed <= 10
            and math.isclose(speed * 10, round(speed * 10), rel_tol=0, abs_tol=1e-9))


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


class EventTail:
    """Follow the appended journal from its current tail; retain recent records and per-kind counts.

    With tracing enabled the journal is dominated by per-step trace records, so the bounded file tail alone
    hides progression. The follower reads only newly appended bytes and never rescans the file.
    """

    def __init__(self, path, recent=500, progression=2000, start_at_end=True):
        self.path = Path(path)
        self.lock = threading.Lock()
        self.recent = deque(maxlen=recent)
        self.progression = deque(maxlen=progression)
        self.kinds = Counter()
        self.minute = deque()
        self.since = int(time.time() * 1000)
        self.identity = None
        self.offset = 0
        self.partial = b''
        self.start_at_end = start_at_end
        self.seeding = False

    def poll(self):
        try:
            with self.path.open('rb') as source:
                stat = os.fstat(source.fileno())
                identity = (stat.st_dev, stat.st_ino)
                if identity != self.identity or stat.st_size < self.offset:
                    self.identity = identity
                    self.partial = b''
                    self.seeding = True
                    # A replaced or truncated journal starts a fresh follow; seed from the existing tail.
                    self.offset = max(0, stat.st_size - 262144) if self.start_at_end else 0
                    if self.offset:
                        source.seek(self.offset)
                        skipped = source.readline()
                        self.offset += len(skipped)
                source.seek(self.offset)
                data = source.read(4 * 1024 * 1024)
        except OSError:
            return
        if not data:
            return
        self.offset += len(data)
        lines = (self.partial + data).split(b'\n')
        self.partial = lines.pop()
        now = time.time()
        # The seed batch restores recent records for display; counts only cover records appended afterwards.
        counted, self.seeding = not self.seeding, False
        with self.lock:
            for line in lines:
                try:
                    value = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(value, dict) or not isinstance(value.get('kind'), str):
                    continue
                self.recent.append(value)
                if counted:
                    self.kinds[value['kind']] += 1
                    self.minute.append((now, value['kind']))
                if value['kind'] not in TRACE_KINDS:
                    self.progression.append(value)
            while self.minute and now - self.minute[0][0] > 60:
                self.minute.popleft()

    def events(self, scope):
        with self.lock:
            return list(self.progression if scope == 'progression' else self.recent)

    def stats(self):
        with self.lock:
            cutoff = time.time() - 60
            recent = Counter(kind for stamp, kind in self.minute if stamp >= cutoff)
            return {'since': self.since, 'total': sum(self.kinds.values()), 'kinds': dict(self.kinds),
                    'recent': dict(recent)}

    def follow(self, stop, interval=0.25):
        while not stop.wait(interval):
            self.poll()


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
    def __init__(self, directory, token, follow=True):
        self.directory = Path(directory)
        self.token = token
        self.lock = threading.RLock()
        self.sequence = 0
        self.max_speed = None
        self.backlog_limit_ms = 100
        self.speed_status = 'Manual speed'
        self.viewers = threading.BoundedSemaphore(16)
        self.tail = EventTail(self.directory / 'events.ndjson')
        self.stop = threading.Event()
        if follow:
            threading.Thread(target=self.tail.follow, args=(self.stop,), daemon=True).start()
            threading.Thread(target=self.follow_speed, daemon=True).start()

    def close(self):
        self.stop.set()

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
            file = spool.directory / path.rsplit('/', 1)[-1]
            try:
                # Fixed content length gives an export cutoff even while the journal grows.
                with file.open('rb') as source:
                    remaining = os.fstat(source.fileno()).st_size
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/x-ndjson' if file.suffix == '.ndjson'
                                     else 'application/json')
                    self.send_header('Content-Length', str(remaining))
                    self.send_header('Content-Disposition', f'attachment; filename="{file.name}"')
                    self.end_headers()
                    while remaining:
                        chunk = source.read(min(65536, remaining))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        remaining -= len(chunk)
            except OSError:
                self.close_connection = True
        elif path == '/api/events':
            scope = parse_qs(urlparse(self.path).query).get('scope', [''])[0]
            if scope == 'progression':
                self.reply(200, spool.tail.events('progression'))
            else:
                self.reply(200, journal_tail(spool.directory / 'events.ndjson', 500, 262144))
        elif path == '/api/event-stats':
            self.reply(200, spool.tail.stats())
        elif path == '/api/history':
            self.reply(200, journal_tail(spool.directory / 'snapshots.ndjson', 1000, 4 * 1024 * 1024))
        else:
            self.reply(404, {'error': 'Not found'})

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
    args = parser.parse_args()
    if not args.token_file.exists():
        descriptor = os.open(args.token_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, 'w') as file:
            file.write(secrets.token_urlsafe(32))
    token = args.token_file.read_text().strip()
    if len(token) < 32:
        parser.error('Token must contain at least 32 characters')
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.daemon_threads = True
    server.spool = Spool(args.spool, token)
    server.maps = args.maps
    print(f'Observatory: http://127.0.0.1:{args.port}; token: {args.token_file}')
    server.serve_forever()


if __name__ == '__main__':
    main()
