"""Wire format shared with the firmware (shared/OvercookedComm/OvercookedComm.h).

Every station <-> bridge message is a type id plus a small packed
little-endian payload. tests/test_protocol.py parses the C++ header and checks
that ids and sizes here still match it.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

PROTOCOL_VERSION = 2
MAX_UID = 10
TAG_SIZE = 11  # size byte + 10 uid bytes


class MsgType(IntEnum):
    HELLO = 0
    HEARTBEAT = 1
    TAG_PLACED = 2
    TAG_REMOVED = 3
    TASK_PROGRESS = 4
    TASK_DONE = 5
    WELCOME = 6
    ACCEPT = 7
    REJECT = 8
    SET_DISPLAY = 9


class StationKind(IntEnum):
    CUTTING_BOARD = 0
    PAN = 1
    PLATE = 3
    DELIVERY = 4
    FRYER = 5
    SINK = 6


class TaskKind(IntEnum):
    NONE = 0
    PRESSES = 1
    JOYSTICK_PATTERN = 2
    FRY = 3
    SCRUB = 4


class DisplayMode(IntEnum):
    IDLE = 0
    CALIBRATED = 1
    BURNT = 4
    SUCCESS = 5
    REJECT = 6
    DISCONNECTED = 7
    GAME_OVER = 8
    PLATE_CLEAN = 9
    PLATE_DIRTY = 10
    PATTERN_CUE = 11  # level = pattern id to do now; +4 blinks it (time running out)


# Names used in level.toml and the UI.
KIND_NAMES = {
    StationKind.CUTTING_BOARD: "cutting_board",
    StationKind.PAN: "pan",
    StationKind.PLATE: "plate",
    StationKind.DELIVERY: "delivery",
    StationKind.FRYER: "deep_fryer",
    StationKind.SINK: "sink",
}
KIND_BY_NAME = {name: kind for kind, name in KIND_NAMES.items()}


def pack_tag(uid: bytes) -> bytes:
    if not 0 < len(uid) <= MAX_UID:
        raise ValueError(f"uid must be 1-{MAX_UID} bytes, got {len(uid)}")
    return bytes([len(uid)]) + uid.ljust(MAX_UID, b"\0")


def unpack_tag(data: bytes) -> bytes:
    size = data[0]
    if size > MAX_UID:
        raise ValueError(f"bad uid size {size}")
    return bytes(data[1 : 1 + size])


# ---- Messages ---------------------------------------------------------------
# Each class has TYPE, a default RELIABLE (which the sender may override), and
# pack() / unpack().


@dataclass(frozen=True)
class Hello:
    TYPE = MsgType.HELLO
    RELIABLE = True
    kind: StationKind
    fw_version: int = 1

    def pack(self) -> bytes:
        return struct.pack("<BB", self.kind, self.fw_version)

    @classmethod
    def unpack(cls, data: bytes) -> "Hello":
        kind, fw = struct.unpack("<BB", data)
        return cls(StationKind(kind), fw)


@dataclass(frozen=True)
class Heartbeat:
    TYPE = MsgType.HEARTBEAT
    RELIABLE = False
    kind: StationKind

    def pack(self) -> bytes:
        return struct.pack("<B", self.kind)

    @classmethod
    def unpack(cls, data: bytes) -> "Heartbeat":
        return cls(StationKind(struct.unpack("<B", data)[0]))


@dataclass(frozen=True)
class TagPlaced:
    TYPE = MsgType.TAG_PLACED
    RELIABLE = True
    uid: bytes

    def pack(self) -> bytes:
        return pack_tag(self.uid)

    @classmethod
    def unpack(cls, data: bytes) -> "TagPlaced":
        return cls(unpack_tag(data))


@dataclass(frozen=True)
class TagRemoved:
    TYPE = MsgType.TAG_REMOVED
    RELIABLE = True
    uid: bytes
    progress: int = 0

    def pack(self) -> bytes:
        return pack_tag(self.uid) + struct.pack("<H", self.progress)

    @classmethod
    def unpack(cls, data: bytes) -> "TagRemoved":
        return cls(unpack_tag(data[:TAG_SIZE]), struct.unpack("<H", data[TAG_SIZE:])[0])


@dataclass(frozen=True)
class TaskProgress:
    TYPE = MsgType.TASK_PROGRESS
    RELIABLE = False
    uid: bytes
    value: int

    def pack(self) -> bytes:
        return pack_tag(self.uid) + struct.pack("<H", self.value)

    @classmethod
    def unpack(cls, data: bytes) -> "TaskProgress":
        return cls(unpack_tag(data[:TAG_SIZE]), struct.unpack("<H", data[TAG_SIZE:])[0])


@dataclass(frozen=True)
class TaskDone:
    TYPE = MsgType.TASK_DONE
    RELIABLE = True
    uid: bytes

    def pack(self) -> bytes:
        return pack_tag(self.uid)

    @classmethod
    def unpack(cls, data: bytes) -> "TaskDone":
        return cls(unpack_tag(data))


@dataclass(frozen=True)
class Welcome:
    TYPE = MsgType.WELCOME
    RELIABLE = True
    protocol_version: int = PROTOCOL_VERSION

    def pack(self) -> bytes:
        return struct.pack("<B", self.protocol_version)

    @classmethod
    def unpack(cls, data: bytes) -> "Welcome":
        return cls(struct.unpack("<B", data)[0])


@dataclass(frozen=True)
class Accept:
    TYPE = MsgType.ACCEPT
    RELIABLE = True
    uid: bytes
    task: TaskKind = TaskKind.NONE
    goal: int = 0
    progress: int = 0
    param: int = 0

    def pack(self) -> bytes:
        return pack_tag(self.uid) + struct.pack("<BHHB", self.task, self.goal, self.progress, self.param)

    @classmethod
    def unpack(cls, data: bytes) -> "Accept":
        task, goal, progress, param = struct.unpack("<BHHB", data[TAG_SIZE:])
        return cls(unpack_tag(data[:TAG_SIZE]), TaskKind(task), goal, progress, param)


@dataclass(frozen=True)
class Reject:
    TYPE = MsgType.REJECT
    RELIABLE = True
    uid: bytes

    def pack(self) -> bytes:
        return pack_tag(self.uid)

    @classmethod
    def unpack(cls, data: bytes) -> "Reject":
        return cls(unpack_tag(data))


@dataclass(frozen=True)
class SetDisplay:
    TYPE = MsgType.SET_DISPLAY
    RELIABLE = False
    mode: DisplayMode
    level: int = 0

    def pack(self) -> bytes:
        return struct.pack("<BB", self.mode, self.level)

    @classmethod
    def unpack(cls, data: bytes) -> "SetDisplay":
        mode, level = struct.unpack("<BB", data)
        return cls(DisplayMode(mode), level)


Message = (
    Hello | Heartbeat | TagPlaced | TagRemoved | TaskProgress | TaskDone
    | Welcome | Accept | Reject | SetDisplay
)

_BY_TYPE = {
    cls.TYPE: cls
    for cls in (
        Hello, Heartbeat, TagPlaced, TagRemoved, TaskProgress, TaskDone,
        Welcome, Accept, Reject, SetDisplay,
    )
}

# Payload sizes, checked against the firmware header by the tests.
SIZES = {
    "TagId": TAG_SIZE,
    "HelloMsg": 2,
    "HeartbeatMsg": 1,
    "TagPlacedMsg": 11,
    "TagRemovedMsg": 13,
    "TaskProgressMsg": 13,
    "TaskDoneMsg": 11,
    "WelcomeMsg": 1,
    "AcceptMsg": 17,
    "RejectMsg": 11,
    "SetDisplayMsg": 2,
}


def decode(msg_type: int, payload: bytes) -> Message:
    """Payload bytes -> message. Raises ValueError on an unknown type or bad size."""
    try:
        cls = _BY_TYPE[MsgType(msg_type)]
    except (KeyError, ValueError):
        raise ValueError(f"unknown message type {msg_type}") from None
    expected = SIZES[cls.__name__ + "Msg"]
    if len(payload) != expected:  # the firmware drops wrong-sized payloads too
        raise ValueError(f"{cls.__name__} payload is {len(payload)} bytes, expected {expected}")
    try:
        return cls.unpack(payload)
    except (struct.error, ValueError, IndexError) as e:
        raise ValueError(f"bad {cls.__name__} payload {payload.hex()}: {e}") from None


def encode(msg: Message) -> tuple[int, bytes]:
    return int(msg.TYPE), msg.pack()
