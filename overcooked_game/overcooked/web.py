"""HTTP + websocket front end, and the glue between the bridge link and the Game."""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
from pathlib import Path
from typing import Callable

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from . import protocol as p
from .engine import Game
from .link import BridgeLink, LineTransport
from .sim import SimBridge

log = logging.getLogger(__name__)

STATIC = Path(__file__).parent / "static"
TICK_S = 0.1
RECONNECT_S = 2.0


class BridgeRunner:
    """Keeps a BridgeLink alive (reconnecting when the USB cable is replugged),
    feeds its events to the Game and gives the Game a send function."""

    def __init__(self, game: Game, transport_factory: Callable[[], LineTransport]):
        self.game = game
        self._factory = transport_factory
        self._link: BridgeLink | None = None

    def send(self, mac: str | None, msg: p.Message, reliable: bool | None = None) -> None:
        if self._link is not None:
            self._link.send(mac, msg, reliable)

    async def run(self) -> None:
        while True:
            try:
                self._link = BridgeLink(self._factory())
                async for event in self._link.events():
                    self.game.handle(event)
            except asyncio.CancelledError:
                raise
            except Exception as e:  # port missing / unplugged
                log.error("bridge link: %s", e)
            self._link = None
            self.game.set_bridge_connected(False)
            await asyncio.sleep(RECONNECT_S)


def create_app(game: Game, runner: BridgeRunner, sim: SimBridge | None = None) -> FastAPI:
    clients: set[WebSocket] = set()

    def state() -> dict:
        snap = game.snapshot()
        snap["type"] = "state"
        snap["sim"] = sim.snapshot() if sim else None
        return snap

    async def ticker() -> None:
        while True:
            if sim:
                sim.step(game.now())
            game.tick()
            if clients:
                text = json.dumps(state())
                for ws in list(clients):
                    try:
                        await ws.send_text(text)
                    except Exception:
                        clients.discard(ws)
            await asyncio.sleep(TICK_S)

    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        tasks = [asyncio.create_task(runner.run()), asyncio.create_task(ticker())]
        try:
            yield
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

    app = FastAPI(title="Overcooked", lifespan=lifespan)

    def run_action(message: dict) -> str | None:
        name = str(message.get("action", ""))
        if name.startswith("sim_"):
            if sim is None:
                return "not running in simulator mode"
            args = {k: v for k, v in message.items() if k in ("mac", "tag", "n")}
            return sim.action(name[4:], **args)
        return game.action(name)

    @app.get("/api/state")
    def get_state() -> dict:
        return state()

    @app.websocket("/ws")
    async def websocket(ws: WebSocket) -> None:
        await ws.accept()
        clients.add(ws)
        try:
            await ws.send_text(json.dumps(state()))
            while True:
                message = json.loads(await ws.receive_text())
                error = run_action(message)
                if error:
                    await ws.send_text(json.dumps({"type": "error", "text": error}))
        except (WebSocketDisconnect, json.JSONDecodeError):
            pass
        finally:
            clients.discard(ws)

    @app.get("/")
    def index() -> FileResponse:
        return FileResponse(STATIC / "index.html")

    app.mount("/static", StaticFiles(directory=STATIC), name="static")
    return app
