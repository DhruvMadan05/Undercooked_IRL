"""Shared test setup: a small level and a Harness that drives a Game with a fake
clock and records what the server sent to stations."""

from __future__ import annotations

import random
import tomllib

import pytest

from overcooked import protocol as p
from overcooked.config import parse_level
from overcooked.engine import Game
from overcooked.link import LocalTag, StationMessage
from overcooked.model import Phase

LEVEL_TOML = """
[level]
duration_s = 60
countdown_s = 3
order_interval_s = 20
max_orders = 2
order_time_s = 40
expire_penalty = 10
time_bonus = 20
respawn_s = 3
plate_capacity = 3

[stations]
cutting_board = 1
pan = 1
deep_fryer = 1
plate = 1
delivery = 1
sink = 1

[ingredient.tomato]
count = 2
cutting_board = { goal = 5 }

[ingredient.patty]
count = 1
pan = { goal = 3, pattern = "zigzag" }

[ingredient.bun]
count = 1

[ingredient.potato]
count = 1
deep_fryer = { goal = 4 }

[recipe.sandwich]
needs = ["tomato:chopped", "bun:raw"]
points = 100

[recipe.bowl]
needs = ["potato:cooked", "patty:cooked"]
points = 50
"""

MASTER = bytes([0xCA, 0xFE, 0x00, 0x01])
TOMATO1 = bytes([0x10, 0x00, 0x00, 0x01])
TOMATO2 = bytes([0x10, 0x00, 0x00, 0x02])
PATTY = bytes([0x20, 0x00, 0x00, 0x01])
BUN = bytes([0x40, 0x00, 0x00, 0x01])
POTATO = bytes([0x60, 0x00, 0x00, 0x01])
PLATE = bytes([0x50, 0x00, 0x00, 0x01])
STRAY = bytes([0xEE, 0xEE, 0xEE, 0xEE])

MACS = {
    p.StationKind.CUTTING_BOARD: "020000000001",
    p.StationKind.PAN: "020000000101",
    p.StationKind.PLATE: "020000000301",
    p.StationKind.DELIVERY: "020000000401",
    p.StationKind.FRYER: "020000000501",
    p.StationKind.SINK: "020000000601",
}
CUT, PAN, PLT, DEL, FRY, SNK = (MACS[k] for k in p.StationKind)


@pytest.fixture
def level():
    return parse_level(tomllib.loads(LEVEL_TOML))


class Harness:
    def __init__(self, level, tmp_path=None):
        self.t = 1000.0
        self.sent: list[tuple[str | None, p.Message, bool | None]] = []
        self.game = Game(
            level,
            send=lambda mac, msg, reliable: self.sent.append((mac, msg, reliable)),
            now=lambda: self.t,
            rng=random.Random(1),
            calibration_path=tmp_path / "calibration.json" if tmp_path else None,
        )

    # -- events in --
    def station_says(self, mac, msg):
        self.game.handle(StationMessage(mac, msg))

    def connect_all(self):
        for kind, mac in MACS.items():
            self.station_says(mac, p.Hello(kind))

    def place(self, mac, uid):
        self.station_says(mac, p.TagPlaced(uid))

    def remove(self, mac, uid, progress=0):
        self.station_says(mac, p.TagRemoved(uid, progress))

    def progress(self, mac, uid, value):
        self.station_says(mac, p.TaskProgress(uid, value))

    def done(self, mac, uid):
        self.station_says(mac, p.TaskDone(uid))

    def bridge_tag(self, uid):
        self.game.handle(LocalTag(uid))

    def advance(self, seconds, step=0.1):
        end = self.t + seconds
        while self.t < end - 1e-9:
            self.t += step
            self.game.tick()

    # -- what was sent --
    def to(self, mac, msg_type=None):
        return [m for (dest, m, _r) in self.sent if dest == mac and (msg_type is None or isinstance(m, msg_type))]

    def last(self, mac, msg_type):
        found = self.to(mac, msg_type)
        return found[-1] if found else None

    def clear(self):
        self.sent.clear()

    # -- shortcuts --
    def calibrate(self):
        """The full calibration sequence, ending in READY."""
        self.connect_all()
        self.game.action("start_calibration")
        self.bridge_tag(MASTER)
        for mac in MACS.values():
            self.place(mac, MASTER)
            self.remove(mac, MASTER)
        for uid in (TOMATO1, TOMATO2, PATTY, BUN, POTATO, PLATE):
            self.bridge_tag(uid)
        assert self.game.phase == Phase.READY, self.game.phase

    def start_round(self):
        self.calibrate()
        assert self.game.action("start_game") is None
        self.advance(3.2)
        assert self.game.phase == Phase.PLAYING, self.game.phase

    def item(self, uid):
        return self.game.items[uid]


@pytest.fixture
def h(level, tmp_path):
    return Harness(level, tmp_path)
