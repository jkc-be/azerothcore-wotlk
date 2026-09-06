#!/usr/bin/env python3
"""Separate-server controller; records measured throughput without assuming 10x is achievable."""
import argparse
import json
import os
from pathlib import Path
import platform
import time
from urllib.request import Request, urlopen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8787')
    parser.add_argument('--token-file', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--hours', type=float, default=24)
    parser.add_argument('--speed', type=int, choices=[1, 2, 5, 10], default=10)
    parser.add_argument('--bots', type=int, default=100)
    parser.add_argument('--control-proof', action='store_true')
    parser.add_argument('--timeout-hours', type=float, default=30)
    args = parser.parse_args()
    token = args.token_file.read_text().strip()
    args.output.mkdir(parents=True, exist_ok=False)
    hardware = {'platform': platform.platform(), 'processor': platform.processor(), 'logicalCPUs': os.cpu_count(),
                'cpuinfo': Path('/proc/cpuinfo').read_text() if Path('/proc/cpuinfo').exists() else None,
                'meminfo': Path('/proc/meminfo').read_text() if Path('/proc/meminfo').exists() else None,
                'measurementHost': platform.node(), 'note': 'Run this controller on the game server for hardware attribution.'}
    (args.output / 'hardware.json').write_text(json.dumps(hardware, indent=2) + '\n')
    deadline = time.monotonic() + args.timeout_hours * 3600
    def api(path, data=None):
        if time.monotonic() > deadline:
            raise TimeoutError('Benchmark wall-clock deadline exceeded')
        request = Request(args.url + path, data=json.dumps(data).encode() if data is not None else None,
                          headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
        with urlopen(request, timeout=10) as response:
            return json.load(response)
    def control(frame, paused, speed):
        result = api('/api/control', {'run': frame['run'], 'paused': paused, 'speed': speed})
        until = time.monotonic() + 10
        while time.monotonic() < until:
            current = api('/api/snapshot')
            if current['run'] != frame['run']:
                raise RuntimeError('Run changed during control request')
            if current['controlSeq'] == result['sequence']:
                if current['paused'] != paused or current['requestedSpeed'] != speed:
                    raise RuntimeError('Control acknowledgement has wrong state')
                return current
            time.sleep(0.1)
        raise TimeoutError('World did not acknowledge the control')
    result = {'status': 'running', 'requestedSpeed': args.speed, 'targetBots': args.bots, 'targetHours': args.hours}
    try:
        while True:
            frame = api('/api/snapshot')
            if frame['fault']:
                raise RuntimeError(frame['fault'])
            if (frame['ready'] and not frame.get('populationPending', False)
                    and frame['expectedBots'] == args.bots and frame['activeBots'] == args.bots
                    and len(frame['bots']) == args.bots):
                break
            time.sleep(1)
        run = frame['run']
        cohort = {bot['id'] for bot in frame['bots']}
        if args.control_proof:
            paused = control(frame, True, 1)
            time.sleep(2)
            after = api('/api/snapshot')
            if after['simMs'] != paused['simMs'] or after['bots'] != paused['bots'] or after['seq'] <= paused['seq']:
                raise RuntimeError('Pause did not freeze gameplay with fresh observation')
            for speed in (1, 2, 5, 10):
                frame = control(after, False, speed)
                time.sleep(1)
            result['controlProof'] = 'passed'
        first = control(frame, False, args.speed)
        last = first
        fresh = time.monotonic()
        samples = 0
        shortfalls = 0
        with (args.output / 'observed.ndjson').open('w') as output:
            while last['simMs'] - first['simMs'] < args.hours * 3600000:
                frame = api('/api/snapshot')
                if frame['run'] != run or frame['fault']:
                    raise RuntimeError('Run changed or faulted')
                if frame['seq'] != last['seq']:
                    fresh = time.monotonic()
                    output.write(json.dumps(frame) + '\n')
                    output.flush()
                    samples += 1
                    shortfalls += frame['achievedSpeed'] < args.speed * 0.95
                    if (frame['expectedBots'] != args.bots or frame['onlineBots'] != args.bots
                            or frame['activeBots'] != args.bots
                            or any(bot['id'] not in cohort for bot in frame['bots'])):
                        raise RuntimeError('Cohort member missing or AI update stalled')
                if time.monotonic() - fresh > 10:
                    raise TimeoutError('World snapshots stopped advancing')
                if frame['completed']:
                    raise RuntimeError('Configured duration ended before requested benchmark window')
                last = frame
                time.sleep(0.5)
        last = control(last, True, args.speed)
        achieved = (last['simMs'] - first['simMs']) / (last['realMs'] - first['realMs'])
        result.update(status='completed', run=run, achievedSpeed=achieved, samples=samples,
                      shortfallSamples=shortfalls, targetReached=achieved >= args.speed * 0.95,
                      correctness='pending controlled trace assessment')
    except Exception as error:
        result.update(status='failed', error=str(error))
        raise
    finally:
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
