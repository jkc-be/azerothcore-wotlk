#!/usr/bin/env python3
"""Summarize real run artifacts; compare trace windows of equal simulated duration."""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import statistics


def records(path):
    with Path(path).open() as source:
        for line in source:
            if line.strip():
                yield json.loads(line)


def summary(directory):
    first = last = None
    faults = set()
    pauses = 0
    stalls = set()
    max_backlog = 0
    low_population = 0
    speeds = []
    for frame in records(Path(directory) / 'snapshots.ndjson'):
        if first is None and frame['ready']:
            first = frame
        last = frame
        if frame['fault']:
            faults.add(frame['fault'])
        max_backlog = max(max_backlog, frame['backlogMs'])
        pauses += frame['paused']
        if frame['ready'] and frame['onlineBots'] != frame['expectedBots']:
            low_population += 1
        if frame['ready'] and not frame['paused']:
            speeds.append(frame['achievedSpeed'])
            for bot in frame['bots']:
                if frame['simMs'] - bot['lastAiMs'] > 10000:
                    stalls.add(bot['id'])
    if first is None or last is None:
        raise ValueError('No complete cohort was observed')
    sim_ms = last['simMs'] - first['simMs']
    real_ms = last['realMs'] - first['realMs']
    return {'run': last['run'], 'simulatedHoursAfterCohortReady': sim_ms / 3600000,
            'realSecondsAfterCohortReady': real_ms / 1000,
            'achievedIncludingPauses': sim_ms / real_ms if real_ms else 0,
            'medianActiveSpeed': statistics.median(speeds) if speeds else 0,
            'maxBacklogMs': max_backlog, 'faults': sorted(faults), 'pausedSnapshots': pauses,
            'populationShortfallSnapshots': low_population, 'aiStalls': sorted(stalls),
            'performanceTargetMeasured': sim_ms >= 86400000,
            'correctness': 'requires controlled trace comparison and server validation'}


def trace(directory, start, duration):
    counts = Counter()
    damage = defaultdict(list)
    cast_starts = defaultdict(list)
    casts = defaultdict(list)
    cooldowns = defaultdict(list)
    positions = {}
    distances = defaultdict(float)
    dead = {}
    respawns = []
    for event in records(Path(directory) / 'events.ndjson'):
        if not start <= event['simMs'] < start + duration:
            continue
        kind, bot = event['kind'], event['bot']
        counts[kind] += 1
        spell = str(event.get('spell', 0))
        key = (bot, spell)
        if kind in ('damage', 'periodic_damage'):
            damage[f'{kind}/{spell}'].append(event['value'])
        elif kind == 'cast_start':
            cast_starts[key].append(event['simMs'])
        elif kind in ('cast_finish', 'cast_cancel') and cast_starts[key]:
            started = cast_starts[key].pop()
            if kind == 'cast_finish':
                casts[spell].append(event['simMs'] - started)
        elif kind == 'cooldown':
            cooldowns[spell].append(event['value'])
        elif kind == 'teleport_attempt':
            positions.pop(bot, None)
        elif kind == 'position':
            previous = positions.get(bot)
            if previous and (previous['map'], previous['instance']) == (event['map'], event['instance']):
                distances[bot] += math.dist([previous[k] for k in ('x', 'y', 'z')],
                                           [event[k] for k in ('x', 'y', 'z')])
            positions[bot] = event
        elif kind == 'creature_death':
            dead[bot] = event['simMs']
        elif kind == 'respawn' and bot in dead:
            respawns.append(event['simMs'] - dead.pop(bot))
    def describe(values):
        return {'count': len(values), 'min': min(values), 'max': max(values), 'mean': statistics.mean(values)}
    return {'window': {'startMs': start, 'durationMs': duration}, 'counts': dict(counts),
            'damagePerHit': {key: describe(value) for key, value in damage.items()},
            'castDurationsMs': {key: describe(value) for key, value in casts.items()},
            'cooldownDurationsMs': {key: describe(value) for key, value in cooldowns.items()},
            'movementYards': dict(distances), 'respawnDurationsMs': respawns,
            'validTimingEvidence': bool(damage and casts and cooldowns and positions),
            'comparisonPolicy': 'Inspect equal scenarios; random encounters are not deterministic equality tests.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--compare', type=Path)
    parser.add_argument('--start-ms', type=int, default=0)
    parser.add_argument('--duration-ms', type=int, default=600000)
    args = parser.parse_args()
    result = {'summary': summary(args.directory)}
    if args.compare:
        result['comparison'] = {'baseline': trace(args.directory, args.start_ms, args.duration_ms),
                                'accelerated': trace(args.compare, args.start_ms, args.duration_ms)}
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
