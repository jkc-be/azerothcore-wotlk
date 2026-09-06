"""Transport-independent actions, observations and event accounting for live/sim adapters."""

from dataclasses import dataclass, field
from math import isfinite
from types import MappingProxyType
from typing import Mapping, Protocol

OBSERVATION_FIELDS = ("health_fraction", "power_fraction", "alive", "combat", "casting")
SPELL_CAST_OK = 255


@dataclass(frozen=True)
class Action:
    operation: str
    arguments: Mapping[str, str] = field(default_factory=dict)

    def __post_init__(self):
        object.__setattr__(self, "arguments", MappingProxyType(dict(self.arguments)))
        schemas = {
            "observe": set(), "stop": set(),
            "move_to": {"x", "y", "z"}, "cast": {"spell", "target"},
        }
        if self.operation not in schemas or set(self.arguments) != schemas[self.operation]:
            raise ValueError("invalid action or arguments")
        if self.operation == "move_to":
            if not all(isfinite(float(value)) for value in self.arguments.values()):
                raise ValueError("coordinates must be finite")
        if self.operation == "cast":
            for name, bits in (("spell", 32), ("target", 64)):
                value = self.arguments[name]
                if not str(value).isascii() or not str(value).isdecimal() or not 0 < int(value) < 2**bits:
                    raise ValueError(f"{name} must be a positive uint{bits}")

    @classmethod
    def move_to(cls, x, y, z):
        return cls("move_to", {"x": str(x), "y": str(y), "z": str(z)})

    @classmethod
    def cast(cls, spell, target):
        return cls("cast", {"spell": str(spell), "target": str(target)})


@dataclass(frozen=True)
class Events:
    kills: int = 0
    deaths: int = 0
    levels: int = 0

    def since(self, previous):
        values = tuple(getattr(self, key) - getattr(previous, key) for key in ("kills", "deaths", "levels"))
        if min(values) < 0:
            raise ValueError("event counters moved backwards")
        return Events(*values)


@dataclass(frozen=True)
class Observation:
    guid: str
    registration: int
    episode: int
    world_tick: int
    elapsed_ms: int
    health: int
    max_health: int
    power: int
    max_power: int
    power_type: int
    level: int
    map_id: int
    instance_id: int
    position: tuple[float, float, float, float]
    alive: bool
    combat: bool
    casting: bool
    events: Events

    @classmethod
    def from_response(cls, response):
        data = response["observation"]
        for key in ("alive", "combat", "casting"):
            if type(data[key]) is not bool:
                raise ValueError(f"{key} must be a JSON boolean")
        position = tuple(float(value) for value in data["position"])
        if len(position) != 4 or not all(isfinite(value) for value in position):
            raise ValueError("invalid position")
        return cls(
            guid=data["guid"], registration=int(data["registration"]), episode=int(data["episode"]),
            world_tick=int(response["world_tick"]), elapsed_ms=int(response["elapsed_ms"]),
            health=int(data["health"]), max_health=int(data["max_health"]),
            power=int(data["power"]), max_power=int(data["max_power"]), power_type=int(data["power_type"]),
            level=int(data["level"]), map_id=int(data["map"]), instance_id=int(data["instance"]),
            position=position, alive=data["alive"], combat=data["combat"], casting=data["casting"],
            events=Events(**{key: int(data["events"][key]) for key in ("kills", "deaths", "levels")}),
        )

    def features(self):
        """Stable minimal vector, in OBSERVATION_FIELDS order. Coordinates stay available separately."""
        return (
            self.health / max(1, self.max_health), self.power / max(1, self.max_power),
            float(self.alive), float(self.combat), float(self.casting),
        )


@dataclass(frozen=True)
class Transition:
    observation: Observation
    events: Events
    spell_result: int

    @property
    def terminated(self):
        return not self.observation.alive


def transition(previous: Observation, current: Observation, spell_result=SPELL_CAST_OK):
    """Cumulative counters survive skipped observations without replaying old rewards."""
    if (previous.guid, previous.registration, previous.episode) != (
        current.guid, current.registration, current.episode
    ):
        raise ValueError("bot or episode changed; establish a new observation baseline")
    if current.world_tick <= previous.world_tick or current.elapsed_ms < previous.elapsed_ms:
        raise ValueError("stale observation")
    return Transition(current, current.events.since(previous.events), spell_result)


class BotEnvironment(Protocol):
    """Implement the same contract for another backend; reward policy belongs to the caller."""

    def reset(self) -> Observation: ...

    def step(self, action: Action, wait_ticks: int = 1) -> Transition: ...
