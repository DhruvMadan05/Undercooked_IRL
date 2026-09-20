"""The USB-serial link to the bridge ESP32.

Line protocol (see overcooked_server/src/main.cpp):
  bridge -> host   BRIDGE <mac12> <protocol> | RX <mac12> <type> <hex> |
                   TXFAIL <mac12> <type> | TAG <uid hex> | PONG | LOG <text>
  host -> bridge   TX <mac12|*> <type> <R|U> <hex> | PING | INFO

BridgeLink speaks that over any LineTransport: a real serial port, or the
simulator (sim.py) which mimics the bridge in memory.
"""

from __future__ import annotations

import asyncio
import logging
import threading
from dataclasses import dataclass
from typing import AsyncIterator, Protocol

from . import protocol as p

log = logging.getLogger(__name__)

BROADCAST = "FFFFFFFFFFFF"


# ---- Events from the bridge ---------------------------------------------------

@dataclass(frozen=True)
class StationMessage:
    mac: str
    msg: p.Message


@dataclass(frozen=True)
class LocalTag:
    """A tag put on the bridge's own reader (calibration)."""
    uid: bytes


@dataclass(frozen=True)
class SendFailed:
    mac: str
    msg_type: int


@dataclass(frozen=True)
class BridgeInfo:
    mac: str
    protocol: int


@dataclass(frozen=True)
class BridgeLog:
    text: str


@dataclass(frozen=True)
class Pong:
    pass


Event = StationMessage | LocalTag | SendFailed | BridgeInfo | BridgeLog | Pong


# ---- Line codec ---------------------------------------------------------------

def format_tx(mac: str | None, msg: p.Message, reliable: bool | None = None) -> str:
    """Line for the bridge to send msg to one station (mac) or everyone (None)."""
    if reliable is None:
        reliable = msg.RELIABLE
    msg_type, payload = p.encode(msg)
    dest = "*" if mac is None else mac.upper()
    return f"TX {dest} {msg_type} {'R' if reliable else 'U'} {payload.hex().upper()}"


def parse_line(line: str) -> Event | None:
    """One bridge line -> event. None for lines that are noise or malformed
    (the ESP32 prints boot messages on the same port)."""
    line = line.strip()
    if not line:
        return None
    head, _, rest = line.partition(" ")
    try:
        if head == "RX":
            mac, msg_type, *payload = rest.split(" ")
            return StationMessage(mac.upper(), p.decode(int(msg_type), bytes.fromhex(payload[0]) if payload else b""))
        if head == "TAG":
            return LocalTag(bytes.fromhex(rest))
        if head == "TXFAIL":
            mac, msg_type = rest.split(" ")
            return SendFailed(mac.upper(), int(msg_type))
        if head == "BRIDGE":
            mac, version = rest.split(" ")
            return BridgeInfo(mac.upper(), int(version))
        if head == "PONG":
            return Pong()
        if head == "LOG":
            return BridgeLog(rest)
    except ValueError as e:
        log.warning("bad bridge line %r: %s", line, e)
        return None
    log.debug("ignoring bridge line %r", line)
    return None


# ---- Transports ---------------------------------------------------------------

class LineTransport(Protocol):
    def write_line(self, line: str) -> None: ...
    async def read_line(self) -> str | None:
        """Next line, or None once the transport is closed."""
    def close(self) -> None: ...


class SerialTransport:
    """pyserial port read on a helper thread, handed to asyncio through a queue."""

    def __init__(self, port: str, baud: int = 115200):
        import serial  # imported here so the simulator works without a port

        self._serial = serial.Serial(port, baud, timeout=0.2)
        self._lines: asyncio.Queue[str | None] = asyncio.Queue()
        self._loop = asyncio.get_running_loop()
        self._closed = threading.Event()
        self._write_lock = threading.Lock()
        self._thread = threading.Thread(target=self._read_loop, name="bridge-serial", daemon=True)
        self._thread.start()

    def _read_loop(self) -> None:
        try:
            while not self._closed.is_set():
                raw = self._serial.readline()
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                self._loop.call_soon_threadsafe(self._lines.put_nowait, text)
        except Exception as e:  # port unplugged
            log.error("serial read failed: %s", e)
        finally:
            self._loop.call_soon_threadsafe(self._lines.put_nowait, None)

    def write_line(self, line: str) -> None:
        with self._write_lock:
            try:
                self._serial.write((line + "\n").encode())
            except Exception as e:
                log.error("serial write failed: %s", e)

    async def read_line(self) -> str | None:
        return await self._lines.get()

    def close(self) -> None:
        self._closed.set()
        self._thread.join(timeout=1)
        self._serial.close()


# ---- The link -----------------------------------------------------------------

class BridgeLink:
    def __init__(self, transport: LineTransport):
        self._transport = transport

    def send(self, mac: str | None, msg: p.Message, reliable: bool | None = None) -> None:
        """Send msg to a station (mac 'AABBCCDDEEFF') or, with mac None, everyone."""
        self._transport.write_line(format_tx(mac, msg, reliable))

    def ping(self) -> None:
        self._transport.write_line("PING")

    def request_info(self) -> None:
        self._transport.write_line("INFO")

    async def events(self) -> AsyncIterator[Event]:
        self.request_info()
        while True:
            line = await self._transport.read_line()
            if line is None:
                return
            event = parse_line(line)
            if event is not None:
                yield event

    def close(self) -> None:
        self._transport.close()
