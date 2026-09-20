"""level.toml -> validated dataclasses."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .model import ItemState
from .protocol import KIND_BY_NAME

# Joystick patterns the pan firmware understands (Accept.param).
PATTERN_IDS = {"circle": 0, "zigzag": 1, "hold": 2, "shake": 3, "press": 4, "flick": 5}

# (from, to) when level.toml does not say.
DEFAULT_STATES = {
    "cutting_board": (ItemState.RAW, ItemState.CHOPPED),
    "pan": (ItemState.RAW, ItemState.COOKED),
    "deep_fryer": (ItemState.RAW, ItemState.COOKED),
}


class ConfigError(ValueError):
    pass


@dataclass(frozen=True)
class Process:
    """What one kind of station does to one ingredient."""
    station: str
    from_state: ItemState
    to_state: ItemState
    goal: int = 0                   # presses / correct gestures (task stations)
    patterns: tuple[str, ...] = ()  # pan only: gestures it asks for, empty = all the firmware knows
    seconds: float = 0.0            # pan only: time to land all goal gestures before it burns
    bonus_s: float = 0.0            # pan only: time given back per correct gesture, times the chain length
    bonus_cap_s: float = 0.0        # pan only: most a single gesture can give back


@dataclass(frozen=True)
class Ingredient:
    name: str
    count: int
    processes: dict[str, Process] = field(default_factory=dict)


@dataclass(frozen=True)
class Recipe:
    name: str
    needs: tuple[str, ...]  # sorted "ingredient:state"
    points: int
    time_s: float


@dataclass(frozen=True)
class Level:
    duration_s: float
    countdown_s: float
    order_interval_s: float
    max_orders: int
    order_time_s: float
    expire_penalty: int
    dump_penalty: int
    wash_s: float
    time_bonus: int
    respawn_s: float
    plate_capacity: int
    stations: dict[str, int]
    plates: int  # = stations["plate"]: each plate reader is one plate with one tag
    ingredients: dict[str, Ingredient]
    recipes: dict[str, Recipe]


def _state(value: str, where: str) -> ItemState:
    try:
        return ItemState(value)
    except ValueError:
        raise ConfigError(f"{where}: unknown state {value!r}") from None


def _process(ingredient: str, station: str, raw: dict) -> Process:
    where = f"ingredient.{ingredient}.{station}"
    default_from, default_to = DEFAULT_STATES[station]
    patterns = raw.get("patterns", [])
    if isinstance(patterns, str):
        patterns = [patterns]
    proc = Process(
        station=station,
        from_state=_state(raw.get("from", default_from.value), where),
        to_state=_state(raw.get("to", default_to.value), where),
        goal=int(raw.get("goal", 0)),
        patterns=tuple(str(pt) for pt in patterns),
        seconds=float(raw.get("seconds", 0)),
        bonus_s=float(raw.get("bonus_s", 0)),
        bonus_cap_s=float(raw.get("bonus_cap_s", 0)),
    )
    if station in ("cutting_board", "pan", "deep_fryer") and proc.goal <= 0:
        raise ConfigError(f"{where}: goal must be > 0")
    if station == "pan":
        unknown = [pt for pt in proc.patterns if pt not in PATTERN_IDS]
        if unknown:
            raise ConfigError(f"{where}: unknown pattern {unknown[0]!r} (known: {', '.join(PATTERN_IDS)})")
    if station == "pan" and proc.seconds <= 0:
        raise ConfigError(f"{where}: seconds must be > 0")
    return proc


def parse_level(data: dict) -> Level:
    lv = data.get("level", {})
    if "plates" in data:
        raise ConfigError("[plates] is gone: every plate reader is a plate, set [stations] plate = N")

    stations = {name: int(n) for name, n in data.get("stations", {}).items()}
    for name in stations:
        if name not in KIND_BY_NAME:
            raise ConfigError(f"[stations]: unknown station {name!r} (known: {', '.join(KIND_BY_NAME)})")

    ingredients: dict[str, Ingredient] = {}
    for name, raw in data.get("ingredient", {}).items():
        raw = dict(raw)
        count = int(raw.pop("count", 1))
        if count < 1:
            raise ConfigError(f"ingredient.{name}: count must be >= 1")
        processes = {}
        for station, params in raw.items():
            if station not in DEFAULT_STATES:
                raise ConfigError(
                    f"ingredient.{name}: unknown key {station!r} (use count or {', '.join(DEFAULT_STATES)})"
                )
            processes[station] = _process(name, station, params)
        ingredients[name] = Ingredient(name, count, processes)

    wash_s = float(lv.get("wash_s", 5))
    if not 0 < wash_s <= 60:  # goes to the station as milliseconds in 16 bits
        raise ConfigError(f"level.wash_s: must be more than 0 and at most 60, got {wash_s}")

    order_time = float(lv.get("order_time_s", 90))
    recipes: dict[str, Recipe] = {}
    for name, raw in data.get("recipe", {}).items():
        needs = tuple(sorted(raw["needs"]))
        for need in needs:
            ingredient, _, state = need.partition(":")
            if ingredient not in ingredients:
                raise ConfigError(f"recipe.{name}: unknown ingredient {ingredient!r}")
            _state(state, f"recipe.{name}")
        recipes[name] = Recipe(name, needs, int(raw.get("points", 100)), float(raw.get("time_s", order_time)))

    return Level(
        duration_s=float(lv.get("duration_s", 180)),
        countdown_s=float(lv.get("countdown_s", 3)),
        order_interval_s=float(lv.get("order_interval_s", 25)),
        max_orders=int(lv.get("max_orders", 4)),
        order_time_s=order_time,
        expire_penalty=int(lv.get("expire_penalty", 10)),
        dump_penalty=int(lv.get("dump_penalty", 5)),
        wash_s=wash_s,
        time_bonus=int(lv.get("time_bonus", 20)),
        respawn_s=float(lv.get("respawn_s", 3)),
        plate_capacity=int(lv.get("plate_capacity", 4)),
        stations=stations,
        plates=stations.get("plate", 0),
        ingredients=ingredients,
        recipes=recipes,
    )


def load_level(path: str | Path) -> Level:
    with open(path, "rb") as f:
        return parse_level(tomllib.load(f))
