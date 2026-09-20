"""A pretend bridge with virtual stations, so the whole game can be run and
tested without hardware.

SimBridge implements the same LineTransport as the real serial port and speaks
the bridge's line protocol, so the real BridgeLink, protocol codec and Game are
all exercised. Each SimStation follows the same rules as the station firmware
(shared/StationCore/src/Station.cpp): announce with Hello until welcomed, then
heartbeat; TagPlaced -> wait for Accept / Reject; report progress and TaskDone;
TagRemoved with its progress.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass

from . import protocol as p
from .config import Level

HEARTBEAT_S = 1.0
TIMEOUT_S = 3.5
SIM_BRIDGE_MAC = "020000000001"


class SimStation:
    def __init__(self, bridge: "SimBridge", mac: str, kind: p.StationKind):
        self.bridge, self.mac, self.kind = bridge, mac, kind
        self.powered = True
        self.connected = False
        self.tag: bytes | None = None
        self.state = "empty"  # empty | awaiting | active | passive | rejected | finished
        self.goal = 0
        self.progress = 0
        self._last_heard = 0.0
        self._next_send = 0.0

    # -- sending to the server (RX lines) --
    def _tx(self, msg: p.Message) -> None:
        msg_type, payload = p.encode(msg)
        self.bridge.emit(f"RX {self.mac} {msg_type} {payload.hex().upper()}")

    # -- messages from the server --
    def on_message(self, msg: p.Message, now: float) -> None:
        if not self.powered:
            return
        if isinstance(msg, p.Welcome):
            self.connected = True
            self._last_heard = now
            self._next_send = now
            if self.tag is not None:  # a server that just appeared knows nothing about our tag
                self._announce()
            return
        if self.connected:
            self._last_heard = now
        if isinstance(msg, p.Accept) and self.state == "awaiting" and msg.uid == self.tag:
            if msg.task == p.TaskKind.NONE:
                self.state = "passive"
            else:
                self.state, self.goal, self.progress = "active", msg.goal, msg.progress
        elif isinstance(msg, p.Accept) and self.state == "active" and msg.uid == self.tag:
            # Re-target, as the firmware does: new goal/pattern, count never goes back.
            self.goal, self.progress = msg.goal, max(self.progress, msg.progress)
        elif isinstance(msg, p.Reject) and self.state == "awaiting" and msg.uid == self.tag:
            self.state = "rejected"

    # -- the player --
    def place(self, uid: bytes) -> None:
        if not self.powered:
            return
        if self.tag is not None:
            self.remove()
        self.tag = uid
        self._announce()

    def remove(self) -> None:
        if self.tag is None:
            return
        progress = self.progress if self.state == "active" else 0
        uid, self.tag, self.state = self.tag, None, "empty"
        if self.powered:
            self._tx(p.TagRemoved(uid, progress))

    def work(self, units: int = 1) -> None:
        """Presses on a cutting board, steps of a pan pattern, ..."""
        if not self.powered or self.state != "active":
            return
        self.progress = min(self.goal, self.progress + units)
        self._tx(p.TaskProgress(self.tag, self.progress))
        if self.progress >= self.goal:
            self._tx(p.TaskDone(self.tag))
            self.state = "finished"

    def _announce(self) -> None:
        self.state, self.progress = "awaiting", 0
        self._tx(p.TagPlaced(self.tag))

    # -- time --
    def step(self, now: float) -> None:
        if not self.powered:
            return
        if self.connected and now - self._last_heard >= TIMEOUT_S:
            self.connected = False
        if now < self._next_send:
            return
        self._next_send = now + HEARTBEAT_S
        self._tx(p.Heartbeat(self.kind) if self.connected else p.Hello(self.kind))


@dataclass
class SimTag:
    name: str
    uid: bytes


class SimBridge:
    """LineTransport + the virtual stations behind it."""

    def __init__(self, level: Level):
        self._lines: asyncio.Queue[str | None] = asyncio.Queue()
        self.stations: dict[str, SimStation] = {}
        for kind_name, count in level.stations.items():
            kind = p.KIND_BY_NAME[kind_name]
            for n in range(count):
                mac = f"02000000{int(kind):02X}{n + 1:02X}"
                self.stations[mac] = SimStation(self, mac, kind)

        self.tags: list[SimTag] = [SimTag("calibration", bytes([0xCA, 0xFE, 0x00, 0x01]))]
        for name, ing in level.ingredients.items():
            self.tags += [SimTag(f"{name} {i + 1}", bytes([0xF0, len(self.tags), 0x00, i])) for i in range(ing.count)]
        self.tags += [SimTag(f"plate {i + 1}", bytes([0xA0, len(self.tags), 0x00, i])) for i in range(level.plates)]
        self.tags.append(SimTag("stray", bytes([0xEE, 0xEE, 0xEE, 0xEE])))
        self._now = 0.0
        self._autocal: asyncio.Task | None = None

    # -- LineTransport --
    def emit(self, line: str) -> None:
        self._lines.put_nowait(line)

    def write_line(self, line: str) -> None:
        cmd, _, rest = line.partition(" ")
        if cmd == "INFO":
            self.emit(f"BRIDGE {SIM_BRIDGE_MAC} {p.PROTOCOL_VERSION}")
        elif cmd == "PING":
            self.emit("PONG")
        elif cmd == "TX":
            mac, msg_type, _mode, *payload = rest.split(" ")
            try:
                msg = p.decode(int(msg_type), bytes.fromhex(payload[0]) if payload else b"")
            except ValueError:
                self.emit("LOG bad TX payload")
                return
            targets = self.stations.values() if mac == "*" else [s for s in [self.stations.get(mac)] if s]
            for station in targets:
                station.on_message(msg, self._now)
        else:
            self.emit("LOG unknown command")

    async def read_line(self) -> str | None:
        return await self._lines.get()

    def close(self) -> None:
        self._lines.put_nowait(None)

    # -- driving it --
    def step(self, now: float) -> None:
        self._now = now
        for station in self.stations.values():
            station.step(now)

    def tag(self, name_or_hex: str) -> SimTag | None:
        return next((t for t in self.tags if t.name == name_or_hex or t.uid.hex() == name_or_hex.lower()), None)

    def tap_bridge_reader(self, tag: SimTag) -> None:
        self.emit(f"TAG {tag.uid.hex().upper()}")

    def snapshot(self) -> dict:
        return {
            "stations": [
                {
                    "mac": s.mac, "kind": p.KIND_NAMES[s.kind], "powered": s.powered,
                    "connected": s.connected, "state": s.state, "progress": s.progress, "goal": s.goal,
                    "tag": next((t.name for t in self.tags if t.uid == s.tag), s.tag.hex() if s.tag else None),
                }
                for s in self.stations.values()
            ],
            "tags": [t.name for t in self.tags],
        }

    def action(self, name: str, mac: str | None = None, tag: str | None = None, n: int = 1) -> str | None:
        """UI buttons of the simulator. Returns an error message or None."""
        station = self.stations.get(mac) if mac else None
        if name == "autocal":
            self._autocal = asyncio.ensure_future(self.auto_calibrate())
            return None
        if name == "tap":
            found = self.tag(tag or "")
            if not found:
                return "unknown tag"
            self.tap_bridge_reader(found)
            return None
        if station is None:
            return "unknown station"
        if name == "place":
            found = self.tag(tag or "")
            if not found:
                return "unknown tag"
            station.place(found.uid)
        elif name == "remove":
            station.remove()
        elif name == "work":
            station.work(n)
        elif name == "power":
            station.powered = not station.powered
            if not station.powered:
                station.connected = False
        else:
            return f"unknown sim action {name!r}"
        return None

    async def auto_calibrate(self, pause: float = 0.15) -> None:
        """Do the whole calibration: calibration tag, each station, every food tag."""
        master = self.tags[0]
        self.tap_bridge_reader(master)
        await asyncio.sleep(pause)
        for station in self.stations.values():
            station.place(master.uid)
            await asyncio.sleep(pause)
            station.remove()
            await asyncio.sleep(pause)
        for tag in self.tags[1:-1]:
            self.tap_bridge_reader(tag)
            await asyncio.sleep(pause)
