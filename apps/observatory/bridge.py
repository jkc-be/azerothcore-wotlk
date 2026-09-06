#!/usr/bin/env python3
"""Local observatory HTTP/SSE adapter. Run on the separate server beside the spool."""
import argparse
import hmac
import json
import os
import re
from pathlib import Path
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

WEB = Path(__file__).with_name('web')
EXPORTS = {'manifest.json', 'initial.json', 'snapshots.ndjson', 'events.ndjson'}


class Spool:
    def __init__(self, directory, token):
        self.directory = Path(directory)
        self.token = token
        self.lock = threading.Lock()
        self.sequence = 0
        self.viewers = threading.BoundedSemaphore(16)

    def snapshot(self):
        return json.loads((self.directory / 'latest.json').read_text())

    def control(self, request):
        if (set(request) != {'run', 'speed', 'paused'}
                or type(request['speed']) is not int or request['speed'] not in (1, 2, 5, 10)
                or type(request['paused']) is not bool):
            raise ValueError('Expected run, speed (1, 2, 5, 10), and paused (boolean)')
        with self.lock:
            current = self.snapshot()
            if request['run'] != current['run']:
                raise ValueError('Run changed; reconnect before controlling')
            if current.get('baseline') and (request['speed'] != 1 or request['paused']):
                raise ValueError('Real-time baseline supports 1x without pause only')
            if current.get('completed'):
                raise ValueError('The configured simulated duration has completed')
            if current['fault']:
                raise ValueError('Run is invalid; inspect server and start a fresh run')
            try:
                run, sequence, _, _ = (self.directory / 'control.txt').read_text().split()
                if run == current['run']:
                    self.sequence = max(self.sequence, int(sequence))
            except (OSError, ValueError):
                pass
            self.sequence = max(self.sequence, current['controlSeq']) + 1
            # The world acknowledges application in subsequent snapshots; HTTP 202 is acceptance only.
            text = f"{request['run']} {self.sequence} {request['speed']} {int(request['paused'])}\n"
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
                  '/model.js': ('model.js', 'text/javascript'), '/style.css': ('style.css', 'text/css')}
        if path in static:
            name, mime = static[path]
            self.reply(200, (WEB / name).read_bytes(), mime)
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
            # Read a bounded tail. Full history stays in the exportable journal.
            try:
                with (spool.directory / 'events.ndjson').open('rb') as source:
                    size = os.fstat(source.fileno()).st_size
                    source.seek(max(0, size - 262144))
                    data = source.read(262144)
                lines = data.splitlines()[1:] if size > 262144 else data.splitlines()
                result = []
                for line in lines[-500:]:
                    try:
                        result.append(json.loads(line))
                    except ValueError:
                        pass  # The writer may still be finishing the last journal record.
                self.reply(200, result)
            except OSError:
                self.reply(200, [])
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
