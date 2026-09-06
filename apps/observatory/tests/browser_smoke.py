#!/usr/bin/env python3
"""Frontend-only Chromium test. Telemetry is synthetic and never represents a measured game server."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

root = Path(__file__).parents[1]
spec = importlib.util.spec_from_file_location('bridge', root / 'bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


def main():
    with tempfile.TemporaryDirectory(prefix='obs-ui-fixture-') as directory:
        spool = Path(directory)
        frame = {'schema': 1, 'run': 'SYNTHETIC-UI-FIXTURE', 'seq': 1, 'simMs': 600000, 'realMs': 60000,
                 'readyAtMs': 0, 'requestedSpeed': 1, 'achievedSpeed': 1, 'paused': False, 'baseline': False,
                 'completed': False, 'controlSeq': 0, 'backlogMs': 0, 'maxTickUs': 500, 'activeBots': 100,
                 'overloaded': False, 'ready': True, 'expectedBots': 100, 'onlineBots': 100,
                 'maxBots': 100, 'populationPending': False, 'fault': '', 'bots': []}
        for i in range(100):
            frame['bots'].append({'id': f'fixture-{i}', 'name': f'FixtureBot{i}', 'map': 0 if i == 0 else 1,
                                 'instance': 0, 'zone': 12, 'x': i * 10, 'y': i * 20, 'z': 0, 'level': i % 10 + 1,
                                 'xp': 20, 'nextLevelXp': 400, 'health': 100, 'maxHealth': 100, 'money': 1000,
                                 'earnedXp': i * 50, 'deaths': i % 3, 'questCompletions': i % 5, 'activity': 'idle',
                                 'lastAiMs': 600000, 'aiUpdates': 100, 'gear': [],
                                 'quests': [{'id': 1, 'state': 0, 'objectives': [2, 0, 0, 0]}]})
        bot_pool = list(frame['bots'])
        (spool / 'events.ndjson').write_text(json.dumps({'run': frame['run'], 'simMs': 600000, 'seq': 1,
            'bot': 'fixture-0', 'kind': 'shortcut', 'detail': 'SYNTHETIC UI TEST: generated gear', 'context': '',
            'value': 0}) + '\n')
        def publish():
            (spool / 'latest.tmp').write_text(json.dumps(frame))
            (spool / 'latest.tmp').replace(spool / 'latest.json')
        publish()
        stopped = threading.Event()
        def updates():
            while not stopped.wait(0.1):
                try:
                    run, sequence, speed, paused, bots = (spool / 'control.txt').read_text().split()
                    frame.update(controlSeq=int(sequence), requestedSpeed=int(speed), paused=bool(int(paused)),
                                 expectedBots=int(bots))
                    if not frame['paused']:
                        frame['bots'] = bot_pool[:int(bots)]
                        frame['onlineBots'] = frame['activeBots'] = int(bots)
                    frame['populationPending'] = len(frame['bots']) != int(bots)
                except FileNotFoundError:
                    pass
                frame['seq'] += 1
                frame['simMs'] += 0 if frame['paused'] else 100
                publish()
        thread = threading.Thread(target=updates)
        thread.start()
        server = bridge.ThreadingHTTPServer(('127.0.0.1', 0), bridge.Handler)
        server.daemon_threads = True
        server.spool = bridge.Spool(spool, 'fixture-token')
        serving = threading.Thread(target=server.serve_forever)
        serving.start()
        artifact = root / 'artifacts' / 'local' / 'browser-fixture.png'
        artifact.parent.mkdir(exist_ok=True)
        try:
            subprocess.run(['node', str(Path(__file__).with_suffix('.mjs')),
                            f'http://127.0.0.1:{server.server_port}', str(artifact)], check=True, timeout=40)
        finally:
            stopped.set()
            thread.join()
            server.shutdown()
            server.server_close()
            serving.join()


if __name__ == '__main__':
    main()
