"""python -m overcooked --port /dev/cu.usbserial-XXXX   (real bridge)
python -m overcooked --sim                              (virtual stations)
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

import uvicorn

from .config import ConfigError, load_level
from .engine import Game
from .link import SerialTransport
from .sim import SimBridge
from .web import BridgeRunner, create_app

ROOT = Path(__file__).resolve().parent.parent


def list_ports() -> None:
    from serial.tools import list_ports as lp

    ports = list(lp.comports())
    for port in ports:
        print(f"{port.device}\t{port.description}")
    if not ports:
        print("no serial ports found")


def main() -> int:
    parser = argparse.ArgumentParser(prog="overcooked", description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--port", help="serial port of the bridge ESP32")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--sim", action="store_true", help="run with virtual stations instead of a bridge")
    parser.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    parser.add_argument("--level", type=Path, default=ROOT / "level.toml")
    parser.add_argument("--calibration", type=Path, default=ROOT / "calibration.json")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--web-port", type=int, default=8000)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.list_ports:
        list_ports()
        return 0
    if bool(args.port) == args.sim:
        parser.error("give exactly one of --port or --sim")

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    try:
        level = load_level(args.level)
    except (OSError, ConfigError, KeyError) as e:
        print(f"cannot load {args.level}: {e}", file=sys.stderr)
        return 1

    sim = SimBridge(level) if args.sim else None
    factory = (lambda: sim) if sim else (lambda: SerialTransport(args.port, args.baud))

    holder: dict[str, BridgeRunner] = {}
    game = Game(level, send=lambda mac, msg, reliable: holder["runner"].send(mac, msg, reliable),
                calibration_path=args.calibration)
    holder["runner"] = runner = BridgeRunner(game, factory)

    print(f"Scoreboard: http://{args.host}:{args.web_port}")
    uvicorn.run(create_app(game, runner, sim), host=args.host, port=args.web_port, log_level="warning")
    return 0


if __name__ == "__main__":
    sys.exit(main())
