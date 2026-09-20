from conftest import BUN, CUT, DEL, MACS, MASTER, PAN, PATTY, PLATE, PLT, POT, RICE, STRAY, TOMATO1, TOMATO2

from overcooked import protocol as p
from overcooked.model import Phase


def test_full_sequence(h):
    g = h.game
    h.connect_all()
    assert g.phase == Phase.CAL_MASTER

    h.bridge_tag(MASTER)
    assert g.phase == Phase.CAL_STATIONS and g.master == MASTER

    h.clear()
    h.place(CUT, MASTER)
    assert g.stations[CUT].name == "Cutting board 1"
    assert h.last(CUT, p.SetDisplay).mode == p.DisplayMode.CALIBRATED  # green flash on the station
    assert g.phase == Phase.CAL_STATIONS

    for mac in (PAN, POT, PLT, DEL):
        h.place(mac, MASTER)
    assert g.phase == Phase.CAL_FOOD  # all stations done: moves on by itself

    for uid in (TOMATO1, TOMATO2, PATTY, RICE, BUN):
        h.bridge_tag(uid)
    assert g.phase == Phase.CAL_FOOD  # the plate is still missing
    h.bridge_tag(PLATE)
    assert g.phase == Phase.READY

    assert g.items[TOMATO2].ingredient == "tomato"
    assert g.items[PLATE].is_plate


def test_master_only_taken_in_first_phase(h):
    h.connect_all()
    h.bridge_tag(STRAY)  # not calibrating stations yet: it becomes the master
    assert h.game.master == STRAY
    h.bridge_tag(MASTER)  # ignored, master is set
    assert h.game.master == STRAY


def test_station_rejects_wrong_tag_during_calibration(h):
    h.connect_all()
    h.bridge_tag(MASTER)
    h.clear()
    h.place(CUT, STRAY)
    assert h.last(CUT, p.Reject).uid == STRAY
    assert h.game.stations[CUT].name is None


def test_touching_twice_does_not_use_another_slot(h):
    h.connect_all()
    h.bridge_tag(MASTER)
    h.place(CUT, MASTER)
    h.remove(CUT, MASTER)
    h.clear()
    h.place(CUT, MASTER)
    assert h.last(CUT, p.SetDisplay).mode == p.DisplayMode.CALIBRATED
    assert sum(1 for s in h.game.checklist.slots if s.mac) == 1


def test_two_stations_of_one_kind_get_touch_order_names(tmp_path):
    from conftest import Harness, LEVEL_TOML
    import tomllib
    from overcooked.config import parse_level

    level = parse_level(tomllib.loads(LEVEL_TOML.replace("cutting_board = 1", "cutting_board = 2")))
    h = Harness(level, tmp_path)
    second = "020000000002"
    h.station_says(CUT, p.Hello(p.StationKind.CUTTING_BOARD))
    h.station_says(second, p.Hello(p.StationKind.CUTTING_BOARD))
    h.bridge_tag(MASTER)
    h.place(second, MASTER)
    h.place(CUT, MASTER)
    assert h.game.stations[second].name == "Cutting board 1"
    assert h.game.stations[CUT].name == "Cutting board 2"

    h.clear()
    h.station_says("020000000003", p.Hello(p.StationKind.CUTTING_BOARD))
    h.place("020000000003", MASTER)  # a third board, but the level has two
    assert isinstance(h.last("020000000003", p.Reject), p.Reject)


def test_food_enrolment_rejects_duplicates_and_master(h):
    h.connect_all()
    h.game.action("start_calibration")
    h.bridge_tag(MASTER)
    for mac in MACS.values():
        h.place(mac, MASTER)
    h.bridge_tag(TOMATO1)
    h.bridge_tag(TOMATO1)   # same tag again
    h.bridge_tag(MASTER)    # the calibration tag is not food
    assert len(h.game.items) == 1
    assert h.game.checklist.current_step.name == "tomato"
    assert [e["kind"] for e in h.game.snapshot()["log"]][-2:] == ["warn", "warn"]


def test_skip_stations_and_ingredients(h):
    h.connect_all()
    h.bridge_tag(MASTER)
    h.place(CUT, MASTER)
    assert h.game.action("next") is None
    assert h.game.phase == Phase.CAL_FOOD
    assert h.game.action("next") is None  # skip tomatoes
    assert h.game.checklist.current_step.name == "patty"
    assert h.game.action("finish_calibration") is None
    assert h.game.phase == Phase.READY


def test_save_and_load(h, level, tmp_path):
    from conftest import Harness

    h.calibrate()
    fresh = Harness(level, tmp_path)  # same calibration.json
    assert fresh.game.has_saved_calibration
    assert fresh.game.action("load_calibration") is None
    assert fresh.game.phase == Phase.READY
    assert fresh.game.master == MASTER
    assert fresh.game.items[BUN].ingredient == "bun"
    assert fresh.game.stations[CUT].name == "Cutting board 1"

    fresh.connect_all()
    assert fresh.game.stations[CUT].online


def test_load_without_a_file(level, tmp_path):
    from conftest import Harness

    fresh = Harness(level, tmp_path)
    assert not fresh.game.has_saved_calibration
    assert fresh.game.action("load_calibration") == "No saved calibration"
    assert fresh.game.phase == Phase.CAL_MASTER


def test_recalibrate_clears_everything(h):
    h.calibrate()
    h.game.action("start_calibration")
    assert h.game.phase == Phase.CAL_MASTER
    assert not h.game.items and h.game.master is None
    assert h.game.stations[CUT].name is None


def make_level(tmp_path, plates):
    import tomllib
    from conftest import Harness, LEVEL_TOML
    from overcooked.config import parse_level

    toml = LEVEL_TOML.replace("plate = 1", f"plate = {plates}")
    return Harness(parse_level(tomllib.loads(toml)), tmp_path)


def test_plate_tag_is_paired_with_its_plate_reader(h):
    h.calibrate()
    assert h.item(PLATE).home_mac == PLT
    assert h.game.plate_for(h.game.stations[PLT]) is h.item(PLATE)


def test_each_plate_reader_gets_its_own_plate_in_calibration_order(tmp_path):
    h = make_level(tmp_path, plates=2)
    second = "020000000302"
    plate2 = bytes([0x50, 0x00, 0x00, 0x02])
    h.station_says(PLT, p.Hello(p.StationKind.PLATE))
    h.station_says(second, p.Hello(p.StationKind.PLATE))
    h.bridge_tag(MASTER)
    h.place(second, MASTER)  # touched first, so it is "Plate 1"
    h.place(PLT, MASTER)
    assert h.game.stations[second].name == "Plate 1"
    h.game.action("next")  # skip the remaining stations
    while h.game.checklist.current_step.name != "plate":
        h.game.action("next")  # skip the food, down to the plates
    labels = [st["label"] for st in h.game.snapshot()["calibration"]["steps"] if st["name"] == "plate"]
    assert labels == ["Plate 1 tag", "Plate 2 tag"]

    h.bridge_tag(PLATE)
    h.bridge_tag(plate2)
    assert h.item(PLATE).home_mac == second
    assert h.item(plate2).home_mac == PLT
    assert h.game.phase == Phase.READY


def test_plate_tag_needs_its_reader_calibrated_first(tmp_path):
    h = make_level(tmp_path, plates=1)
    h.station_says(PLT, p.Hello(p.StationKind.PLATE))
    h.bridge_tag(MASTER)
    h.game.action("next")  # skip the stations: the plate reader is not calibrated
    while h.game.checklist.current_step.name != "plate":
        h.game.action("next")
    h.bridge_tag(PLATE)
    assert PLATE not in h.game.items
    assert h.game.snapshot()["log"][-1]["kind"] == "warn"


def test_plate_pairing_survives_save_and_load(h, level, tmp_path):
    from conftest import Harness

    h.calibrate()
    fresh = Harness(level, tmp_path)
    assert fresh.game.action("load_calibration") is None
    assert fresh.game.items[PLATE].home_mac == PLT


def test_old_plates_section_is_rejected():
    import pytest
    from overcooked.config import ConfigError, parse_level

    with pytest.raises(ConfigError, match=r"\[plates\] is gone"):
        parse_level({"plates": {"count": 2}})
