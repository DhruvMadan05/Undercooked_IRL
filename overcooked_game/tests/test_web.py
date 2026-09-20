import time

import pytest
from fastapi.testclient import TestClient

from overcooked.engine import Game
from overcooked.sim import SimBridge
from overcooked.web import BridgeRunner, create_app


@pytest.fixture
def client(level, tmp_path):
    sim = SimBridge(level)
    holder = {}
    game = Game(level, send=lambda m, msg, r: holder["runner"].send(m, msg, r), calibration_path=tmp_path / "cal.json")
    holder["runner"] = runner = BridgeRunner(game, lambda: sim)
    with TestClient(create_app(game, runner, sim)) as c:
        yield c


def test_state_over_http_and_websocket(client):
    snap = client.get("/api/state").json()
    assert snap["phase"] == "cal_master" and snap["sim"]["tags"]

    with client.websocket_connect("/ws") as ws:
        assert ws.receive_json()["type"] == "state"


def test_actions_over_websocket(client):
    with client.websocket_connect("/ws") as ws:
        ws.receive_json()
        ws.send_json({"action": "start_game"})
        message = next(m for m in (ws.receive_json() for _ in range(50)) if m["type"] == "error")
        assert "calibration" in message["text"]

        ws.send_json({"action": "sim_tap", "tag": "calibration"})
        deadline = time.time() + 5
        while time.time() < deadline:
            state = ws.receive_json()
            if state["type"] == "state" and state["phase"] == "cal_stations":
                break
        else:
            pytest.fail("the calibration tag was never picked up")
        assert state["master"] == "cafe0001"


def test_stations_appear_in_the_state(client):
    with client.websocket_connect("/ws") as ws:
        deadline = time.time() + 5
        while time.time() < deadline:
            state = ws.receive_json()
            if state["type"] == "state" and len(state["stations"]) == 7:
                return
        pytest.fail("stations never registered")


def test_index_page(client):
    assert client.get("/").status_code == 200
