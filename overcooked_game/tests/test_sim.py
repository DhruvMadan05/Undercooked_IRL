"""The Game, BridgeLink and the simulator's virtual stations together, talking
through the real bridge line protocol on a fake clock."""

import asyncio

import pytest

from conftest import CUT

from overcooked import protocol as p
from overcooked.engine import Game
from overcooked.link import BridgeLink
from overcooked.model import ItemState, Phase
from overcooked.sim import SimBridge


class Rig:
    def __init__(self, level, tmp_path):
        self.t = 5000.0
        self.sim = SimBridge(level)
        self.link = BridgeLink(self.sim)
        self.game = Game(level, send=self.link.send, now=lambda: self.t, calibration_path=tmp_path / "cal.json")
        self.pump = asyncio.create_task(self._pump())

    async def _pump(self):
        async for event in self.link.events():
            self.game.handle(event)

    async def settle(self):
        for _ in range(40):
            await asyncio.sleep(0)

    async def run(self, seconds, step=0.1):
        for _ in range(round(seconds / step)):
            self.t += step
            self.sim.step(self.t)
            self.game.tick()
            await self.settle()

    def station(self, mac):
        return self.sim.stations[mac]

    async def close(self):
        self.pump.cancel()
        await asyncio.gather(self.pump, return_exceptions=True)


@pytest.fixture
async def rig(level, tmp_path):
    r = Rig(level, tmp_path)
    yield r
    await r.close()


async def test_stations_handshake(rig):
    await rig.run(2)
    assert rig.game.bridge_mac == "020000000001"
    assert len(rig.game.stations) == 6
    assert all(s.online for s in rig.game.stations.values())
    assert all(s.connected for s in rig.sim.stations.values())


async def test_calibrate_and_play_a_round(rig):
    await rig.run(2)
    await rig.sim.auto_calibrate(pause=0)
    await rig.settle()
    assert rig.game.phase == Phase.READY
    assert all(s.name for s in rig.game.stations.values())

    assert rig.game.action("start_game") is None
    await rig.run(3.5)
    assert rig.game.phase == Phase.PLAYING

    tomato = rig.sim.tag("tomato 1")
    board = rig.station(CUT)
    board.place(tomato.uid)
    await rig.settle()
    assert (board.state, board.goal) == ("active", 5)

    board.work(3)
    board.remove()          # picked up half way
    await rig.settle()
    assert rig.game.items[tomato.uid].progress == 3

    board.place(tomato.uid)  # put back: resumes
    await rig.settle()
    assert (board.state, board.progress) == ("active", 3)
    board.work(2)
    await rig.settle()
    assert board.state == "finished"
    assert rig.game.items[tomato.uid].state == ItemState.CHOPPED

    board.remove()
    board.place(rig.sim.tag("stray").uid)
    await rig.settle()
    assert board.state == "rejected"


async def test_station_that_loses_the_server_reconnects(rig):
    await rig.run(2)
    board = rig.station(CUT)
    assert board.connected
    board.powered = False
    await rig.run(5)
    assert not rig.game.stations[CUT].online
    board.powered = True
    await rig.run(3)
    assert rig.game.stations[CUT].online and board.connected


async def test_sim_snapshot_and_actions(rig):
    await rig.run(1)
    snap = rig.sim.snapshot()
    assert {s["kind"] for s in snap["stations"]} == {"cutting_board", "pan", "deep_fryer", "plate", "delivery", "sink"}
    assert rig.sim.action("tap", tag="calibration") is None
    assert rig.sim.action("tap", tag="nope") == "unknown tag"
    assert rig.sim.action("place", mac="nope", tag="stray") == "unknown station"
    await rig.settle()
    assert rig.game.master is not None
