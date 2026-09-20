"""The game: calibration, tags and stations, orders, score and the round clock.

Game is pure logic. It is fed events (handle), a clock (tick) and UI actions
(action), and sends messages to stations through the `send` callable it was
given. It never touches serial, the network or wall-clock time directly, so
tests can drive it with a fake clock.
"""

from __future__ import annotations

import logging
import random
import time
from collections import deque
from pathlib import Path
from typing import Callable

from . import calibration
from . import protocol as p
from .config import Level
from .kinds import BEHAVIORS
from .link import BridgeInfo, BridgeLog, Event, LocalTag, Pong, SendFailed, StationMessage
from .model import Item, ItemState, Order, Phase, Station

log = logging.getLogger(__name__)

# send(mac, message, reliable): reliable None = the message's default.
Sender = Callable[[str | None, p.Message, bool | None], None]

STATION_TIMEOUT_S = 3.5  # matches the firmware's Session timeout
LOG_KEEP = 60


class Game:
    def __init__(
        self,
        level: Level,
        send: Sender,
        now: Callable[[], float] = time.monotonic,
        rng: random.Random | None = None,
        calibration_path: Path | None = None,
    ):
        self.level = level
        self._send = send
        self.now = now
        self.rng = rng or random.Random()
        self.calibration_path = calibration_path

        self.phase = Phase.CAL_MASTER
        self.stations: dict[str, Station] = {}
        self.items: dict[bytes, Item] = {}
        self.master: bytes | None = None
        self.checklist = calibration.Checklist(level)

        self.score = 0
        self.delivered = 0
        self.orders: list[Order] = []
        self._order_ids = 0
        self._next_order_at = 0.0
        self._countdown_until = 0.0
        self._ends_at = 0.0

        self.bridge_mac: str | None = None
        self.bridge_connected = False
        self._last_tick = now()
        self._log: deque[dict] = deque(maxlen=LOG_KEEP)
        self._log_id = 0

    # ---- helpers used by station behaviours ------------------------------------

    def log(self, text: str, kind: str = "info") -> None:
        self._log_id += 1
        self._log.append({"id": self._log_id, "t": round(self.now(), 2), "kind": kind, "text": text})
        log.log(logging.WARNING if kind in ("warn", "burn") else logging.INFO, text)

    def send(self, station: Station, msg: p.Message, reliable: bool | None = None) -> None:
        self._send(station.mac, msg, reliable)

    def accept(self, station: Station, item: Item, task: p.TaskKind = p.TaskKind.NONE,
               goal: int = 0, progress: int = 0, param: int = 0) -> None:
        station.accepted = item.uid
        self.send(station, p.Accept(item.uid, task, goal, progress, param))

    def reject(self, station: Station, item: Item | bytes | None, reason: str) -> None:
        uid = item.uid if isinstance(item, Item) else item
        station.accepted = None
        if uid:
            self.send(station, p.Reject(uid))
        self.log(f"{station.label}: {reason}", "warn")

    def flash(self, station: Station, mode: p.DisplayMode) -> None:
        """One-shot LED effect. Reliable, because there is no refresh for it."""
        self.send(station, p.SetDisplay(mode), reliable=True)

    def match_order(self, plate: Item) -> Order | None:
        key = tuple(sorted(e.key for e in plate.contents))
        matches = [o for o in self.orders if o.needs == key]
        return min(matches, key=lambda o: o.deadline, default=None)

    def complete_order(self, order: Order, plate: Item) -> None:
        now = self.now()
        span = max(order.deadline - order.created, 1e-6)
        bonus = round(self.level.time_bonus * max(0.0, order.deadline - now) / span)
        self.score += order.points + bonus
        self.delivered += 1
        self.orders.remove(order)
        self.log(f"Delivered {order.recipe}: +{order.points + bonus}", "score")
        for entry in plate.contents:
            self._schedule_respawn(self.items[entry.uid])
        plate.contents.clear()

    def trash(self, item: Item) -> None:
        self.log(f"{item.label} thrown away", "info")
        self._schedule_respawn(item)

    def _schedule_respawn(self, item: Item) -> None:
        item.state = ItemState.CONSUMED
        item.progress, item.progress_kind = 0, None
        item.respawn_at = self.now() + self.level.respawn_s

    # ---- events in --------------------------------------------------------------

    def handle(self, event: Event) -> None:
        self.bridge_connected = True  # anything at all from the bridge proves it is there
        if isinstance(event, StationMessage):
            self._on_station_message(event.mac, event.msg)
        elif isinstance(event, LocalTag):
            self._on_local_tag(event.uid)
        elif isinstance(event, SendFailed):
            self._on_send_failed(event)
        elif isinstance(event, BridgeInfo):
            self.bridge_mac = event.mac
            self.bridge_connected = True
            if event.protocol != p.PROTOCOL_VERSION:
                self.log(f"Bridge speaks protocol {event.protocol}, server {p.PROTOCOL_VERSION}: reflash", "error")
            else:
                self.log(f"Bridge connected ({event.mac})", "ok")
        elif isinstance(event, BridgeLog):
            self.log(f"bridge: {event.text}", "info")
        elif isinstance(event, Pong):
            self.bridge_connected = True

    def set_bridge_connected(self, connected: bool) -> None:
        if self.bridge_connected and not connected:
            self.log("Bridge disconnected", "error")
        self.bridge_connected = connected

    def _on_send_failed(self, event: SendFailed) -> None:
        station = self.stations.get(event.mac)
        name = p.MsgType(event.msg_type).name if event.msg_type in p.MsgType._value2member_map_ else event.msg_type
        self.log(f"{station.label if station else event.mac}: {name} not delivered", "warn")

    # ---- station messages -------------------------------------------------------

    def _on_station_message(self, mac: str, msg: p.Message) -> None:
        now = self.now()
        station = self.stations.get(mac)

        if isinstance(msg, (p.Hello, p.Heartbeat)):
            first_contact = station is None
            station = self._register(mac, msg.kind, now)
            if isinstance(msg, p.Hello) or first_contact:
                # A station that says hello has just (re)booted or lost us, and
                # a heartbeat from a station we do not know means we restarted:
                # either way whatever we thought was on it is unknown now. The
                # Welcome makes it announce the tag that is on it again.
                self._release(station, None)
                station.item = None
                self.send(station, p.Welcome())
                if isinstance(msg, p.Hello):
                    self.log(f"{station.label} says hello", "info")
            self._send_display(station, force=True)
            return

        if station is None:
            # We restarted and this station has not heartbeated yet. It will
            # within a second, and our Welcome then makes it announce this tag again.
            return
        station.last_seen = now
        if not station.online:
            station.online = True

        if isinstance(msg, p.TagPlaced):
            self._on_tag_placed(station, msg.uid)
        elif isinstance(msg, p.TagRemoved):
            if station.item == msg.uid:
                station.item = None
            if station.accepted == msg.uid:
                self._release(station, msg.progress)
        elif isinstance(msg, p.TaskProgress):
            item = self.items.get(msg.uid)
            if item and station.accepted == msg.uid:
                BEHAVIORS[station.kind].on_progress(self, station, item, msg.value)
        elif isinstance(msg, p.TaskDone):
            item = self.items.get(msg.uid)
            if item and station.accepted == msg.uid and self.phase == Phase.PLAYING:
                BEHAVIORS[station.kind].on_done(self, station, item)

    def _register(self, mac: str, kind: p.StationKind, now: float) -> Station:
        station = self.stations.get(mac)
        if station is None:
            station = self.stations[mac] = Station(mac, kind)
        station.kind = kind
        station.last_seen = now
        if not station.online:
            station.online = True
            self.log(f"{station.label} online", "ok")
        return station

    def _release(self, station: Station, progress: int | None) -> None:
        """The accepted tag left (or the station forgot it)."""
        uid, station.accepted = station.accepted, None
        item = self.items.get(uid) if uid else None
        if item:
            BEHAVIORS[station.kind].on_removed(self, station, item, item.progress if progress is None else progress)

    def _on_tag_placed(self, station: Station, uid: bytes) -> None:
        self._release(station, None)  # a lost TagRemoved must not leave a task running
        station.item = uid

        if self.phase == Phase.CAL_STATIONS:
            self._calibrate_station(station, uid)
        elif self.phase == Phase.PLAYING:
            self._place_in_game(station, uid)
        else:
            self.reject(station, uid, f"tag ignored ({self.phase.value})")

    def _place_in_game(self, station: Station, uid: bytes) -> None:
        item = self.items.get(uid)
        if item is None:
            self.reject(station, uid, f"unknown tag {uid.hex()}")
        elif item.state == ItemState.CONSUMED:
            self.reject(station, item, f"{item.label} was used, it comes back shortly")
        else:
            BEHAVIORS[station.kind].on_placed(self, station, item)

    # ---- calibration -------------------------------------------------------------

    def _calibrate_station(self, station: Station, uid: bytes) -> None:
        if uid != self.master:
            self.reject(station, uid, "touch the calibration tag here")
            return
        existing = self.checklist.slot_of(station.mac)
        if existing:
            self.flash(station, p.DisplayMode.CALIBRATED)
            self.log(f"{station.label} is already calibrated", "info")
            return
        kind_name = p.KIND_NAMES[station.kind]
        slot = self.checklist.free_slot(kind_name)
        if slot is None:
            self.reject(station, uid, f"the level has no free {kind_name.replace('_', ' ')} slot")
            return
        slot.mac = station.mac
        station.name = calibration.slot_name(kind_name, slot.index)
        self.flash(station, p.DisplayMode.CALIBRATED)
        self.log(f"Calibrated {station.name} ({station.mac})", "ok")
        if self.checklist.stations_done:
            self._enter_food_calibration("All stations calibrated")

    def _enter_food_calibration(self, why: str) -> None:
        self.phase = Phase.CAL_FOOD
        self.log(f"{why}. Now touch food and plate tags to the server.", "ok")
        if self.checklist.food_done:
            self._finish_calibration()

    def _on_local_tag(self, uid: bytes) -> None:
        if self.phase == Phase.CAL_MASTER:
            self.master = uid
            self.log(f"Calibration tag set ({uid.hex()})", "ok")
            self.phase = Phase.CAL_STATIONS
            if self.checklist.stations_done:
                self._enter_food_calibration("No stations to calibrate")
        elif self.phase == Phase.CAL_FOOD:
            self._enroll(uid)
        else:
            self.log(f"Tag {uid.hex()} on the server reader ignored ({self.phase.value})", "info")

    def _enroll(self, uid: bytes) -> None:
        step = self.checklist.current_step
        if step is None:
            return
        if uid == self.master:
            self.log("That is the calibration tag, use a different tag", "warn")
            return
        if uid in self.items:
            known = self.items[uid]
            self.log(f"Tag {uid.hex()} is already enrolled as {known.label}", "warn")
            return
        self.items[uid] = Item(uid, None if step.name == "plate" else step.name)
        step.uids.append(uid)
        self.log(f"Enrolled {step.name} {len(step.uids)}/{step.total} ({uid.hex()})", "ok")
        if step.done:
            self.checklist.advance()
            if self.checklist.food_done:
                self._finish_calibration()

    def _finish_calibration(self) -> None:
        self.phase = Phase.READY
        self._save_calibration()
        self.log("Calibration done. Press Start when everyone is ready.", "ok")

    def _save_calibration(self) -> None:
        if not self.calibration_path or self.master is None:
            return
        stations = {m: (s.kind, s.name) for m, s in self.stations.items() if s.name}
        items = {uid: it.ingredient for uid, it in self.items.items()}
        try:
            calibration.save(self.calibration_path, self.master, stations, items)
        except OSError as e:
            self.log(f"Could not save calibration: {e}", "warn")

    @property
    def has_saved_calibration(self) -> bool:
        return bool(self.calibration_path and calibration.load(self.calibration_path))

    def _load_calibration(self) -> str | None:
        saved = calibration.load(self.calibration_path) if self.calibration_path else None
        if saved is None:
            return "No saved calibration"
        self.master = saved["master"]
        self.checklist = calibration.Checklist(self.level)
        self.items = {}
        for uid, ingredient in saved["items"]:
            if ingredient is None or ingredient in self.level.ingredients:
                self.items[uid] = Item(uid, ingredient)
            else:
                self.log(f"Saved tag {uid.hex()} is {ingredient}, not in this level: skipped", "warn")
        for mac, kind_name, name in saved["stations"]:
            kind = p.KIND_BY_NAME.get(kind_name)
            slot = self.checklist.free_slot(kind_name)
            if kind is None or slot is None:
                self.log(f"Saved station {name} does not fit this level: skipped", "warn")
                continue
            slot.mac = mac
            station = self.stations.get(mac) or self.stations.setdefault(mac, Station(mac, kind))
            station.name = name
        self.phase = Phase.READY
        self.log(f"Loaded calibration: {len(self.items)} tags, {sum(1 for s in self.checklist.slots if s.mac)} stations", "ok")
        return None

    # ---- UI actions ---------------------------------------------------------------

    def action(self, name: str) -> str | None:
        """Run a UI action. Returns an error message, or None when it worked."""
        handler = getattr(self, f"_action_{name}", None)
        if handler is None:
            return f"unknown action {name!r}"
        return handler()

    def _action_start_calibration(self) -> str | None:
        self.phase = Phase.CAL_MASTER
        self.master = None
        self.items = {}
        self.checklist = calibration.Checklist(self.level)
        self._reset_round_state()
        for station in self.stations.values():
            station.name = None
        self.log("Calibration started: touch a tag to the server to make it the calibration tag", "info")
        return None

    def _action_next(self) -> str | None:
        if self.phase == Phase.CAL_STATIONS:
            self._enter_food_calibration("Skipped remaining stations")
        elif self.phase == Phase.CAL_FOOD:
            self.checklist.advance()
            if self.checklist.food_done:
                self._finish_calibration()
        else:
            return "nothing to skip right now"
        return None

    def _action_finish_calibration(self) -> str | None:
        if self.phase != Phase.CAL_FOOD:
            return "not enrolling food"
        self._finish_calibration()
        return None

    def _action_load_calibration(self) -> str | None:
        if self.phase == Phase.PLAYING or self.phase == Phase.COUNTDOWN:
            return "a round is running"
        return self._load_calibration()

    def _action_start_game(self) -> str | None:
        if self.phase not in (Phase.READY, Phase.ENDED):
            return "finish calibration first"
        self._reset_round_state()
        self.phase = Phase.COUNTDOWN
        self._countdown_until = self.now() + self.level.countdown_s
        return None

    def _action_end_game(self) -> str | None:
        if self.phase != Phase.PLAYING:
            return "no round is running"
        self._end_round()
        return None

    def _action_reset(self) -> str | None:
        if self.phase in (Phase.CAL_MASTER, Phase.CAL_STATIONS, Phase.CAL_FOOD):
            return "calibrate first"
        self._reset_round_state()
        self.phase = Phase.READY
        self.log("Reset: all food is raw again", "info")
        return None

    def _reset_round_state(self) -> None:
        for item in self.items.values():
            item.reset()
        for station in self.stations.values():
            station.accepted = None
            station.data.clear()
        self.orders = []
        self.score = 0
        self.delivered = 0

    # ---- round ---------------------------------------------------------------------

    def _begin_round(self) -> None:
        now = self.now()
        self.phase = Phase.PLAYING
        self._ends_at = now + self.level.duration_s
        self._next_order_at = now
        self.log("Go!", "ok")

    def _end_round(self) -> None:
        self.phase = Phase.ENDED
        self.orders = []
        self.log(f"Time! Final score {self.score} ({self.delivered} delivered)", "score")

    def tick(self) -> None:
        now = self.now()
        dt = min(max(now - self._last_tick, 0.0), 1.0)
        self._last_tick = now

        for station in self.stations.values():
            if station.online and now - station.last_seen > STATION_TIMEOUT_S:
                station.online = False
                self.log(f"{station.label} went offline", "warn")

        if self.phase == Phase.COUNTDOWN and now >= self._countdown_until:
            self._begin_round()
        if self.phase == Phase.PLAYING:
            if now >= self._ends_at:
                self._end_round()
            else:
                self._update_orders(now)
                self._respawn(now)
                for station in self.stations.values():
                    BEHAVIORS[station.kind].tick(self, station, dt)

        for station in self.stations.values():
            self._send_display(station)

    def _update_orders(self, now: float) -> None:
        for order in [o for o in self.orders if now >= o.deadline]:
            self.orders.remove(order)
            self.score = max(0, self.score - self.level.expire_penalty)
            self.log(f"Order {order.recipe} expired: -{self.level.expire_penalty}", "warn")

        room = len(self.orders) < self.level.max_orders
        if self.level.recipes and room and (now >= self._next_order_at or not self.orders):
            self._spawn_order(now)
            self._next_order_at = now + self.level.order_interval_s

    def _spawn_order(self, now: float) -> None:
        recipe = self.rng.choice(list(self.level.recipes.values()))
        self._order_ids += 1
        self.orders.append(Order(self._order_ids, recipe.name, recipe.needs, recipe.points, now, now + recipe.time_s))
        self.log(f"New order: {recipe.name}", "order")

    def _respawn(self, now: float) -> None:
        for item in self.items.values():
            if item.respawn_at is not None and now >= item.respawn_at:
                item.reset()

    # ---- station displays ----------------------------------------------------------

    def _desired_display(self, station: Station) -> tuple[p.DisplayMode, int]:
        if self.phase == Phase.ENDED:
            return p.DisplayMode.GAME_OVER, 0
        if self.phase == Phase.PLAYING:
            return BEHAVIORS[station.kind].display(self, station)
        return p.DisplayMode.IDLE, 0

    def _send_display(self, station: Station, force: bool = False) -> None:
        """Send the persistent display when it changed, or always with force
        (the reply to a heartbeat, which also tells the station we are alive)."""
        if not station.online and not force:
            return
        desired = self._desired_display(station)
        if force or desired != station.sent_display:
            station.sent_display = desired
            self.send(station, p.SetDisplay(*desired))

    # ---- state out -------------------------------------------------------------------

    def snapshot(self) -> dict:
        now = self.now()
        playing = self.phase == Phase.PLAYING
        snap = {
            "phase": self.phase.value,
            "score": self.score,
            "delivered": self.delivered,
            "duration": self.level.duration_s,
            "time_left": max(0.0, self._ends_at - now) if playing else (0.0 if self.phase == Phase.ENDED else self.level.duration_s),
            "countdown": max(0.0, self._countdown_until - now) if self.phase == Phase.COUNTDOWN else None,
            "master": self.master.hex() if self.master else None,
            "bridge": {"connected": self.bridge_connected, "mac": self.bridge_mac},
            "has_saved_calibration": self.has_saved_calibration,
            "orders": [
                {
                    "id": o.id, "recipe": o.recipe, "needs": list(o.needs), "points": o.points,
                    "time_left": max(0.0, o.deadline - now), "total": o.deadline - o.created,
                }
                for o in self.orders
            ],
            "stations": [self._station_snapshot(s) for s in self.stations.values()],
            "items": [
                {"uid": it.uid.hex(), "label": it.label, "state": it.state.value,
                 "contents": [e.key for e in it.contents]}
                for it in self.items.values()
            ],
            "log": list(self._log)[-30:],
        }
        if self.phase in (Phase.CAL_MASTER, Phase.CAL_STATIONS, Phase.CAL_FOOD):
            snap["calibration"] = self.checklist.to_dict()
        return snap

    def _station_snapshot(self, station: Station) -> dict:
        item = self.items.get(station.item) if station.item else None
        snap = {
            "mac": station.mac,
            "kind": p.KIND_NAMES[station.kind],
            "name": station.name,
            "label": station.label,
            "online": station.online,
            "calibrated": station.name is not None,
            "item": (item.label if item else "unknown tag") if station.item else None,
            "item_state": item.state.value if item else None,
        }
        snap.update(BEHAVIORS[station.kind].describe(self, station))
        return snap
