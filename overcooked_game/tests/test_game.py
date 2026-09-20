import pytest

from conftest import BUN, CUT, DEL, MASTER, PAN, PATTY, PLATE, PLT, POT, RICE, STRAY, TOMATO1, TOMATO2

from overcooked import protocol as p
from overcooked.model import ItemState, Phase


def chop(h, uid=TOMATO1):
    """Put a tomato on the board and finish the cutting."""
    h.place(CUT, uid)
    h.done(CUT, uid)


def make_sandwich_plate(h):
    chop(h)
    h.remove(CUT, TOMATO1)
    h.place(PLT, TOMATO1)
    h.remove(PLT, TOMATO1)
    h.place(PLT, BUN)
    h.remove(PLT, BUN)


# ---- round flow ---------------------------------------------------------------------

def test_start_needs_calibration(h):
    assert h.game.action("start_game") == "finish calibration first"


def test_countdown_then_playing_then_time_up(h):
    h.calibrate()
    h.game.action("start_game")
    assert h.game.phase == Phase.COUNTDOWN
    assert 0 < h.game.snapshot()["countdown"] <= 3
    h.advance(3.2)
    assert h.game.phase == Phase.PLAYING
    h.advance(30)
    assert 25 < h.game.snapshot()["time_left"] < 31
    h.advance(31)
    assert h.game.phase == Phase.ENDED
    assert h.game.orders == []


def test_tags_ignored_outside_a_round(h):
    h.calibrate()
    h.clear()
    h.place(CUT, TOMATO1)
    assert isinstance(h.last(CUT, p.Reject), p.Reject)


def test_reset_makes_food_raw_and_zeroes_score(h):
    h.start_round()
    chop(h)
    h.game.score = 50
    assert h.game.action("reset") is None
    assert h.game.phase == Phase.READY
    assert h.item(TOMATO1).state == ItemState.RAW
    assert h.game.score == 0


# ---- cutting board --------------------------------------------------------------------

def test_cutting_flow(h):
    h.start_round()
    h.clear()
    h.place(CUT, TOMATO1)
    accept = h.last(CUT, p.Accept)
    assert (accept.uid, accept.task, accept.goal, accept.progress) == (TOMATO1, p.TaskKind.PRESSES, 5, 0)

    h.progress(CUT, TOMATO1, 3)
    h.done(CUT, TOMATO1)
    assert h.item(TOMATO1).state == ItemState.CHOPPED
    assert h.item(TOMATO1).progress == 0


def test_pick_up_and_resume(h):
    h.start_round()
    h.place(CUT, TOMATO1)
    h.progress(CUT, TOMATO1, 2)
    h.remove(CUT, TOMATO1, progress=3)  # the station reports where it stopped
    assert h.item(TOMATO1).progress == 3

    h.clear()
    h.place(CUT, TOMATO1)
    assert h.last(CUT, p.Accept).progress == 3


def test_progress_survives_a_lost_removal(h):
    h.start_round()
    h.place(CUT, TOMATO1)
    h.progress(CUT, TOMATO1, 4)
    h.place(CUT, TOMATO2)  # TagRemoved never arrived
    assert h.item(TOMATO1).progress == 4
    assert h.last(CUT, p.Accept).uid == TOMATO2


def test_different_tomato_starts_from_zero(h):
    h.start_round()
    h.place(CUT, TOMATO1)
    h.progress(CUT, TOMATO1, 4)
    h.remove(CUT, TOMATO1, 4)
    h.place(CUT, TOMATO2)
    assert h.last(CUT, p.Accept).progress == 0


@pytest.mark.parametrize("uid,why", [
    (STRAY, "unknown tag"),
    (PATTY, "patty cannot be cut"),
    (BUN, "bun cannot be cut"),
    (PLATE, "plates are not food"),
])
def test_cutting_board_rejects(h, uid, why):
    h.start_round()
    h.clear()
    h.place(CUT, uid)
    assert h.last(CUT, p.Reject).uid == uid, why
    assert h.last(CUT, p.Accept) is None


def test_chopped_tomato_is_rejected_again(h):
    h.start_round()
    chop(h)
    h.remove(CUT, TOMATO1)
    h.clear()
    h.place(CUT, TOMATO1)
    assert h.last(CUT, p.Reject) is not None


def test_stale_task_messages_are_ignored(h):
    h.start_round()
    h.done(CUT, TOMATO1)  # nothing was accepted
    h.progress(CUT, TOMATO1, 3)
    assert h.item(TOMATO1).state == ItemState.RAW and h.item(TOMATO1).progress == 0


# ---- pan --------------------------------------------------------------------------------

ZIGZAG, CIRCLE = 1, 0  # the two patterns the test level allows on the pan


def test_pan_asks_for_a_new_gesture_after_each_step(h):
    h.start_round()
    h.place(PAN, PATTY)
    first = h.last(PAN, p.Accept)
    assert (first.task, first.goal, first.progress) == (p.TaskKind.JOYSTICK_PATTERN, 3, 0)
    assert first.param in (ZIGZAG, CIRCLE)

    h.progress(PAN, PATTY, 1)
    second = h.last(PAN, p.Accept)
    assert (second.goal, second.progress) == (3, 1)
    assert second.param != first.param  # never the same one twice running

    h.progress(PAN, PATTY, 2)
    third = h.last(PAN, p.Accept)
    assert (third.progress, third.param) == (2, first.param)

    h.clear()
    h.progress(PAN, PATTY, 3)  # goal reached: nothing more to ask for
    assert h.to(PAN, p.Accept) == []
    h.done(PAN, PATTY)
    assert h.item(PATTY).state == ItemState.COOKED


def test_pan_cue_shows_the_gesture_and_blinks_when_time_is_short(h):
    h.start_round()
    h.place(PAN, PATTY)
    asked = h.last(PAN, p.Accept).param
    assert h.game._desired_display(h.game.stations[PAN]) == (p.DisplayMode.PATTERN_CUE, asked)

    h.advance(17)  # 3 of 20 s left: under the 20% mark
    assert h.game._desired_display(h.game.stations[PAN]) == (p.DisplayMode.PATTERN_CUE, asked + 6)


def test_pan_burns_when_time_runs_out(h):
    h.start_round()
    h.place(PAN, PATTY)
    h.advance(19.5)
    assert h.item(PATTY).state == ItemState.RAW
    h.advance(1)
    assert h.item(PATTY).state == ItemState.BURNT
    assert h.game._desired_display(h.game.stations[PAN])[0] == p.DisplayMode.BURNT
    h.clear()
    h.progress(PAN, PATTY, 1)  # too late, nothing happens
    assert h.to(PAN, p.Accept) == []


def test_pan_correct_gestures_give_time_back(h):
    h.start_round()
    h.place(PAN, PATTY)
    h.advance(10)
    used = h.item(PATTY).pan_ms
    assert 9_500 <= used <= 10_000  # the fake clock's 0.1 s steps round down a little
    h.progress(PAN, PATTY, 1)  # chain 1: 2 s back
    assert h.item(PATTY).pan_ms == used - 2_000
    h.progress(PAN, PATTY, 2)  # chain 2: 4 s, capped at 3
    assert h.item(PATTY).pan_ms == used - 5_000


def test_pan_pauses_and_resumes_when_picked_up(h):
    h.start_round()
    h.place(PAN, PATTY)
    h.advance(5)
    h.progress(PAN, PATTY, 1)
    h.remove(PAN, PATTY, 1)
    h.advance(30)  # off the pan: the clock does not run
    assert h.item(PATTY).state == ItemState.RAW
    h.place(PAN, PATTY)
    accept = h.last(PAN, p.Accept)
    assert accept.progress == 1
    h.advance(19.5)  # only 3 of the 20 s were used before (5 - 2 bonus)
    assert h.item(PATTY).state == ItemState.BURNT


def test_pan_ignores_stale_progress(h):
    h.start_round()
    h.place(PAN, PATTY)
    h.progress(PAN, PATTY, 2)
    h.clear()
    h.progress(PAN, PATTY, 2)  # the same report again (unreliable messages)
    h.progress(PAN, PATTY, 1)
    assert h.to(PAN, p.Accept) == []
    assert h.item(PATTY).progress == 2


def test_pan_rejects_tomato(h):
    h.start_round()
    h.clear()
    h.place(PAN, TOMATO1)
    assert h.last(PAN, p.Reject) is not None


# ---- pot ----------------------------------------------------------------------------------

def test_pot_cooks_then_burns(h):
    h.start_round()
    h.place(POT, RICE)
    assert h.last(POT, p.Accept).task == p.TaskKind.NONE

    h.advance(5)
    assert h.item(RICE).state == ItemState.RAW
    mode, level = h.game._desired_display(h.game.stations[POT])
    assert mode == p.DisplayMode.COOKING and 100 < level < 160

    h.advance(5.5)
    assert h.item(RICE).state == ItemState.COOKED
    assert h.game._desired_display(h.game.stations[POT])[0] == p.DisplayMode.COOKING

    h.advance(6.5)  # 6 s after cooked: past 60% of burn_after
    assert h.game._desired_display(h.game.stations[POT])[0] == p.DisplayMode.WARNING

    h.advance(4.5)
    assert h.item(RICE).state == ItemState.BURNT
    assert h.game._desired_display(h.game.stations[POT])[0] == p.DisplayMode.BURNT


def test_pot_pushes_display_changes_to_the_station(h):
    h.start_round()
    h.place(POT, RICE)
    h.clear()
    h.advance(2)
    displays = h.to(POT, p.SetDisplay)
    assert displays and displays[-1].mode == p.DisplayMode.COOKING
    assert displays[-1].level > displays[0].level or len(displays) == 1
    h.clear()
    h.advance(0.3)
    assert len(h.to(POT, p.SetDisplay)) <= 3  # only on change, not every tick


def test_pot_time_is_kept_when_food_is_taken_out(h):
    h.start_round()
    h.place(POT, RICE)
    h.advance(6)
    h.remove(POT, RICE)
    h.advance(20)  # out of the pot: nothing happens
    assert h.item(RICE).state == ItemState.RAW
    h.place(POT, RICE)
    h.advance(4.5)
    assert h.item(RICE).state == ItemState.COOKED


def test_cooked_food_does_not_go_back_in_the_pot(h):
    h.start_round()
    h.place(POT, RICE)
    h.advance(10.5)
    h.remove(POT, RICE)
    h.clear()
    h.place(POT, RICE)
    assert h.last(POT, p.Reject) is not None


# ---- plate + delivery ------------------------------------------------------------------------

def test_food_goes_on_the_plate_with_no_plate_scan(h):
    h.start_round()
    h.clear()
    h.place(PLT, BUN)
    assert isinstance(h.last(PLT, p.Accept), p.Accept)
    assert [e.key for e in h.item(PLATE).contents] == ["bun:raw"]
    assert h.item(BUN).state == ItemState.CONSUMED


def test_plate_tag_is_not_scanned_at_the_plate_reader(h):
    h.start_round()
    h.clear()
    h.place(PLT, PLATE)
    assert h.last(PLT, p.Reject).uid == PLATE
    assert h.last(PLT, p.Accept) is None


def test_plate_reader_shows_what_is_on_the_plate(h):
    h.start_round()
    h.place(PLT, BUN)
    tile = next(s for s in h.game.snapshot()["stations"] if s["mac"] == PLT)
    assert tile["note"] == "on the plate: bun:raw"


def test_assembly_and_delivery_scores_an_order(h):
    h.start_round()
    h.game.orders.clear()
    g = h.game
    g._spawn_order(h.t)
    g.orders[0] = type(g.orders[0])(1, "sandwich", ("bun:raw", "tomato:chopped"), 100, h.t, h.t + 40)
    h.advance(0.1)

    make_sandwich_plate(h)
    assert [e.key for e in h.item(PLATE).contents] == ["tomato:chopped", "bun:raw"]
    assert h.item(TOMATO1).state == ItemState.CONSUMED

    h.clear()
    h.place(DEL, PLATE)
    assert h.last(DEL, p.Accept) is not None
    assert g.delivered == 1
    assert 100 < g.score <= 120  # points + time bonus (delivered almost instantly)
    assert h.item(PLATE).contents == []

    # the tags recycle after respawn_s
    assert h.item(TOMATO1).state == ItemState.CONSUMED
    h.advance(3.2)
    assert h.item(TOMATO1).state == ItemState.RAW
    assert h.item(BUN).state == ItemState.RAW


def test_delivery_rejects_a_plate_nobody_ordered(h):
    h.start_round()
    h.game.orders.clear()
    make_sandwich_plate(h)
    h.clear()
    h.place(DEL, PLATE)
    assert h.last(DEL, p.Reject) is not None
    assert h.game.score == 0
    assert len(h.item(PLATE).contents) == 2


def test_delivery_rejects_an_empty_plate(h):
    h.start_round()
    h.clear()
    h.place(DEL, PLATE)
    assert h.last(DEL, p.Reject) is not None


def test_burnt_food_cannot_be_plated_but_can_be_trashed(h):
    h.start_round()
    h.place(POT, RICE)
    h.advance(21)
    assert h.item(RICE).state == ItemState.BURNT
    h.remove(POT, RICE)

    h.clear()
    h.place(PLT, RICE)
    assert h.last(PLT, p.Reject) is not None

    h.remove(PLT, RICE)
    h.place(DEL, RICE)  # loose food on the delivery station = bin
    assert h.item(RICE).state == ItemState.CONSUMED
    h.advance(3.2)
    assert h.item(RICE).state == ItemState.RAW


def test_plate_capacity(h):
    h.start_round()
    h.place(CUT, TOMATO1); h.done(CUT, TOMATO1); h.remove(CUT, TOMATO1)
    h.place(CUT, TOMATO2); h.done(CUT, TOMATO2); h.remove(CUT, TOMATO2)
    for uid in (TOMATO1, TOMATO2, BUN):
        h.place(PLT, uid)
        h.remove(PLT, uid)
    assert len(h.item(PLATE).contents) == 3
    h.clear()
    h.place(PLT, PATTY)
    assert h.last(PLT, p.Reject) is not None


# ---- orders ------------------------------------------------------------------------------------

def test_orders_spawn_and_expire(h):
    h.start_round()
    h.advance(0.2)
    assert len(h.game.orders) == 1  # the first one appears at once
    h.game.score = 30
    first = h.game.orders[0]
    h.advance(20.5)
    assert len(h.game.orders) == 2  # a second one after order_interval_s
    h.advance(20)  # the first one (40 s) runs out
    assert first not in h.game.orders
    assert h.game.score == 20  # -10 expire penalty


def test_order_cap(h):
    h.start_round()
    h.advance(50)
    assert len(h.game.orders) <= 2


# ---- stations coming and going -------------------------------------------------------------------

def test_station_goes_offline_and_back(h):
    h.calibrate()
    h.advance(1)
    h.station_says(CUT, p.Heartbeat(p.StationKind.CUTTING_BOARD))
    h.advance(3)
    assert h.game.stations[CUT].online
    h.advance(1)
    assert not h.game.stations[CUT].online
    h.station_says(CUT, p.Heartbeat(p.StationKind.CUTTING_BOARD))
    assert h.game.stations[CUT].online


def test_heartbeat_gets_a_display_reply(h):
    h.connect_all()
    h.clear()
    h.station_says(CUT, p.Heartbeat(p.StationKind.CUTTING_BOARD))
    assert isinstance(h.last(CUT, p.SetDisplay), p.SetDisplay)


def test_hello_is_answered_with_welcome(h):
    h.clear()
    h.station_says(CUT, p.Hello(p.StationKind.CUTTING_BOARD))
    assert isinstance(h.last(CUT, p.Welcome), p.Welcome)


def test_hello_forgets_the_tag_on_a_rebooted_station(h):
    h.start_round()
    h.place(CUT, TOMATO1)
    h.progress(CUT, TOMATO1, 2)
    h.station_says(CUT, p.Hello(p.StationKind.CUTTING_BOARD))
    assert h.game.stations[CUT].accepted is None
    assert h.item(TOMATO1).progress == 2  # progress is kept on the server


def test_station_unknown_after_server_restart(h):
    mac = "0200000000FF"
    h.clear()
    h.place(mac, TOMATO1)  # arrives before the station's first heartbeat
    assert h.sent == []

    h.station_says(mac, p.Heartbeat(p.StationKind.CUTTING_BOARD))
    assert isinstance(h.last(mac, p.Welcome), p.Welcome)  # makes the station announce its tag again


def test_game_over_display(h):
    h.start_round()
    h.advance(61)
    assert h.game._desired_display(h.game.stations[CUT])[0] == p.DisplayMode.GAME_OVER


def test_snapshot_is_json_ready(h):
    import json
    h.start_round()
    h.place(CUT, TOMATO1)
    snap = h.game.snapshot()
    json.dumps(snap)
    tile = next(s for s in snap["stations"] if s["mac"] == CUT)
    assert tile["item"] == "tomato" and tile["online"] and tile["name"] == "Cutting board 1"
