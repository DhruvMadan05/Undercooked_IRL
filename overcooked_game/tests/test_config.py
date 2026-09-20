from pathlib import Path

import pytest

from overcooked.config import ConfigError, load_level, parse_level
from overcooked.model import ItemState

LEVEL_FILE = Path(__file__).resolve().parents[1] / "level.toml"


def test_shipped_level_loads():
    level = load_level(LEVEL_FILE)
    assert level.recipes and level.ingredients and level.stations
    tomato = level.ingredients["tomato"].processes["cutting_board"]
    assert (tomato.from_state, tomato.to_state) == (ItemState.RAW, ItemState.CHOPPED)


def test_states_can_be_overridden():
    level = parse_level({"ingredient": {"veg": {"pan": {"goal": 5, "from": "chopped"}}}})
    proc = level.ingredients["veg"].processes["pan"]
    assert (proc.from_state, proc.to_state) == (ItemState.CHOPPED, ItemState.COOKED)


def test_pot_is_gone():
    with pytest.raises(ConfigError, match="unknown station"):
        parse_level({"stations": {"pot": 1}})
    with pytest.raises(ConfigError, match="unknown key"):
        parse_level({"ingredient": {"rice": {"pot": {"seconds": 5}}}})


def test_wash_time_defaults_and_is_limited():
    assert parse_level({}).wash_s == 5
    assert parse_level({"level": {"wash_s": 2.5}}).wash_s == 2.5
    for bad in (0, -1, 61):
        with pytest.raises(ConfigError, match="wash_s"):
            parse_level({"level": {"wash_s": bad}})


@pytest.mark.parametrize("data,text", [
    ({"stations": {"blender": 1}}, "unknown station"),
    ({"ingredient": {"x": {"cutting_board": {}}}}, "goal must be"),
    ({"ingredient": {"x": {"pan": {"goal": 3, "pattern": "spiral"}}}}, "unknown pattern"),
    ({"ingredient": {"x": {"grill": {"goal": 3}}}}, "unknown key"),
    ({"ingredient": {"x": {"count": 0}}}, "count must be"),
    ({"recipe": {"r": {"needs": ["ghost:raw"]}}}, "unknown ingredient"),
    ({"ingredient": {"x": {}}, "recipe": {"r": {"needs": ["x:frozen"]}}}, "unknown state"),
])
def test_bad_config(data, text):
    with pytest.raises(ConfigError, match=text):
        parse_level(data)
