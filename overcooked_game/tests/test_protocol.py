import re
from pathlib import Path

import pytest

from overcooked import protocol as p
from overcooked.link import (
    BridgeInfo, BridgeLog, LocalTag, Pong, SendFailed, StationMessage, format_tx, parse_line,
)

HEADER = Path(__file__).resolve().parents[2] / "shared/OvercookedComm/src/OvercookedComm.h"
UID = bytes([0xDE, 0xAD, 0xBE, 0xEF])

MESSAGES = [
    p.Hello(p.StationKind.PAN, 3),
    p.Heartbeat(p.StationKind.POT),
    p.TagPlaced(UID),
    p.TagPlaced(bytes(range(1, 11))),
    p.TagRemoved(UID, 123),
    p.TaskProgress(UID, 65535),
    p.TaskDone(UID),
    p.Welcome(),
    p.Accept(UID, p.TaskKind.PRESSES, 200, 17, 1),
    p.Reject(UID),
    p.SetDisplay(p.DisplayMode.COOKING, 200),
]


@pytest.mark.parametrize("msg", MESSAGES, ids=lambda m: type(m).__name__)
def test_round_trip(msg):
    msg_type, payload = p.encode(msg)
    assert p.decode(msg_type, payload) == msg


def test_known_bytes():
    # Golden bytes: pins the layout the firmware has to produce.
    assert p.Accept(UID, p.TaskKind.PRESSES, 200, 17, 1).pack().hex() == "04deadbeef000000000000" "01" "c800" "1100" "01"
    assert p.TagRemoved(UID, 0x0102).pack().hex() == "04deadbeef000000000000" "0201"


def test_decode_rejects_bad_input():
    with pytest.raises(ValueError):
        p.decode(99, b"")
    with pytest.raises(ValueError):
        p.decode(p.MsgType.TAG_PLACED, b"\x04\x01")  # too short
    with pytest.raises(ValueError):
        p.decode(p.MsgType.TAG_PLACED, b"\x0b" + bytes(10))  # uid size 11


def test_uid_length_limits():
    with pytest.raises(ValueError):
        p.pack_tag(b"")
    with pytest.raises(ValueError):
        p.pack_tag(bytes(11))


# ---- Stay in sync with the firmware header ---------------------------------------

def camel_to_snake(name):
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()


@pytest.fixture(scope="module")
def header():
    return HEADER.read_text()


def enum_values(header, enum_name):
    body = re.search(rf"enum class {enum_name} : uint8_t \{{(.*?)\}};", header, re.S).group(1)
    return {camel_to_snake(n): int(v) for n, v in re.findall(r"(\w+)\s*=\s*(\d+)", body)}


@pytest.mark.parametrize("name,enum", [
    ("MsgType", p.MsgType), ("StationKind", p.StationKind),
    ("TaskKind", p.TaskKind), ("DisplayMode", p.DisplayMode),
])
def test_enums_match_firmware(header, name, enum):
    assert enum_values(header, name) == {m.name: int(m) for m in enum}


def test_sizes_match_firmware(header):
    firmware = {n: int(s) for n, s in re.findall(r"static_assert\(sizeof\((\w+)\) == (\d+)", header)}
    assert firmware == p.SIZES
    for msg in MESSAGES:
        assert len(msg.pack()) == p.SIZES[type(msg).__name__ + "Msg"]


def test_reliability_and_version_match_firmware(header):
    for name, reliable in re.findall(r"Payload<MsgType::(\w+)>.*?reliable = (true|false)", header):
        cls = next(m for m in MESSAGES if m.TYPE.name == camel_to_snake(name))
        assert cls.RELIABLE == (reliable == "true"), name
    assert int(re.search(r"kProtocolVersion = (\d+)", header).group(1)) == p.PROTOCOL_VERSION


# ---- Bridge line protocol ---------------------------------------------------------

def test_format_tx():
    assert format_tx("aabbccddeeff", p.Welcome()) == "TX AABBCCDDEEFF 6 R 02"
    assert format_tx(None, p.SetDisplay(p.DisplayMode.BURNT), reliable=True) == "TX * 9 R 0400"
    assert format_tx("AABBCCDDEEFF", p.SetDisplay(p.DisplayMode.IDLE)) == "TX AABBCCDDEEFF 9 U 0000"


def test_parse_lines():
    assert parse_line("RX aabbccddeeff 2 04DEADBEEF000000000000") == StationMessage("AABBCCDDEEFF", p.TagPlaced(UID))
    assert parse_line("TAG DEADBEEF") == LocalTag(UID)
    assert parse_line("TXFAIL AABBCCDDEEFF 7") == SendFailed("AABBCCDDEEFF", 7)
    assert parse_line("BRIDGE AABBCCDDEEFF 2") == BridgeInfo("AABBCCDDEEFF", 2)
    assert parse_line("PONG") == Pong()
    assert parse_line("LOG rc522 not answering") == BridgeLog("rc522 not answering")


@pytest.mark.parametrize("line", [
    "", "   ", "ets Jun  8 2016 00:22:57", "rst:0x1 (POWERON_RESET)", "RX", "RX aabb 99 00",
    "RX aabbccddeeff 2 04DE", "TAG xyz", "TXFAIL onlyone",
])
def test_parse_ignores_noise(line):
    assert parse_line(line) is None


def test_joystick_pattern_ids_match_firmware(header):
    from overcooked.config import PATTERN_IDS

    assert {name.lower(): v for name, v in enum_values(header, "Pattern").items()} == PATTERN_IDS
