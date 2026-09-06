#!/usr/bin/env python3
"""Synthetic browser preview; never connects to a game server or database."""

import argparse
import math
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "modules/mod-python-api/python"))
from acore_api import Action, Events, Observation, ObservatoryPublisher


def publish_demo(publisher, tick):
    for index in range(8):
        phase = tick / 8 + index
        observation = Observation(
            guid=str(9007199254740993 + index), registration=1, episode=0,
            world_tick=tick + 1, elapsed_ms=tick * 250, health=int(65 + 35 * math.sin(phase)),
            max_health=100, power=int(50 + 45 * math.cos(phase)), max_power=100,
            power_type=0, level=10 + index, map_id=0, instance_id=0,
            position=(-9465 + 120 * math.sin(phase), 65 + 120 * math.cos(phase), 56, phase % (2 * math.pi)),
            alive=True, combat=index % 2 == 0, casting=index % 3 == 0,
            events=Events(kills=tick // 20, deaths=0, levels=0),
        )
        publisher.publish(observation, name=f"DemoBot{index + 1}",
                          action=Action("observe") if tick % 20 == 0 else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spool", type=Path, required=True, help="New directory; must not already exist")
    parser.add_argument("--seconds", type=int, default=300, help="Preview duration (default: 300 seconds)")
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("seconds must be positive")
    with ObservatoryPublisher(args.spool, label="SYNTHETIC DEMO · no game server") as publisher:
        print(f"Synthetic observations: {args.spool}", flush=True)
        tick = 0
        deadline = time.monotonic() + args.seconds
        try:
            while time.monotonic() < deadline:
                publish_demo(publisher, tick)
                tick += 1
                time.sleep(0.25)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
