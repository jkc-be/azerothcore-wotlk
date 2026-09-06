#!/usr/bin/env python3
"""Exercise actual bot logins/logouts through a disposable observatory's authenticated API.

Changes the running target and pause state; restores the original controls on success/failure.
Requires an exclusive run, a pool of at least three bots, and no concurrent operator controls.
"""
import argparse
import json
from pathlib import Path
import time
from urllib.request import Request, urlopen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8787')
    parser.add_argument('--token-file', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    token = args.token_file.read_text().strip()

    def api(path, body=None):
        headers = {'Authorization': 'Bearer ' + token}
        if body is not None:
            headers['Content-Type'] = 'application/json'
        request = Request(args.url + path, headers=headers,
                          data=None if body is None else json.dumps(body).encode())
        with urlopen(request, timeout=10) as response:
            return json.load(response)

    original = api('/api/snapshot')
    assert original.get('maxBots', 0) >= 3, 'Provision a pool of at least three bots'
    assert not (original['baseline'] or original['fault'] or original['completed']), 'Run is not controllable'
    samples = []

    def sample():
        frame = api('/api/snapshot')
        assert frame['run'] == original['run'], 'Run changed during validation'
        assert not frame['fault'], frame['fault']
        samples.append(frame)
        return frame

    def wait_for(predicate, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = sample()
            if predicate(frame):
                return frame
            time.sleep(0.25)
        raise AssertionError('Population/AI did not reach the requested state before the real-time deadline')

    def control(bots, paused=False):
        result = api('/api/control', {'run': original['run'], 'speed': 1, 'paused': paused, 'bots': bots})
        return wait_for(lambda f: f['controlSeq'] >= result['sequence'] and f['expectedBots'] == bots
                        and f['paused'] == paused)

    def matched(bots):
        return wait_for(lambda f: not f['populationPending'] and f['onlineBots'] == bots
                        and len(f['bots']) == bots)

    status = 'failed'
    try:
        control(1)
        matched(1)
        frozen = control(1, paused=True)
        control(3, paused=True)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            frame = sample()
            assert frame['simMs'] == frozen['simMs'], 'Gameplay advanced while paused'
            assert frame['onlineBots'] == 1 and frame['populationPending'], 'Paused population mutated'
            time.sleep(0.25)
        control(3)
        three = matched(3)
        before = {b['id']: b['aiUpdates'] for b in three['bots']}
        moving = wait_for(lambda f: len(f['bots']) == 3 and all(
            b['id'] in before and b['aiUpdates'] > before[b['id']] for b in f['bots']))
        assert moving['activeBots'] == 3, 'New bots are not actively simulated without humans'
        control(1)
        one = matched(1)
        assert one['bots'][0]['id'] in before, 'Shrinking unexpectedly replaced the whole cohort'
        control(0)
        empty = matched(0)
        wait_for(lambda f: f['simMs'] > empty['simMs'] + 1000 and not f['bots'])
        control(3)
        matched(3)
        for previous, current in zip(samples, samples[1:]):
            for key in ('xp', 'quests', 'deaths'):
                assert current['runTotals'][key] >= previous['runTotals'][key], 'Departed progression was lost'
        status = 'passed'
        print('PASS: live 1→3→1→0→3, paused resize, active AI, empty-world responsiveness, retained totals', flush=True)
    finally:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({'status': status, 'run': original['run'], 'samples': samples}, indent=2))
        frame = api('/api/snapshot')
        if frame['run'] == original['run'] and not frame['fault']:
            api('/api/control', {'run': original['run'], 'speed': original['requestedSpeed'],
                                 'paused': original['paused'], 'bots': original['expectedBots']})


if __name__ == '__main__':
    main()
