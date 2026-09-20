"""What each kind of station does when a tag is put on it.

The firmware only reports events and measures the player's input; these
classes hold the rules. A behaviour talks to the Game through a small surface:
game.level, game.items, game.accept / reject / flash / log, game.match_order,
game.complete_order, game.trash.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from .config import PATTERN_IDS, Process
from .model import Item, ItemState, PlateEntry, Station
from .protocol import DisplayMode, StationKind, TaskKind

if TYPE_CHECKING:
    from .engine import Game


class Behavior:
    kind: StationKind
    name: str  # key in level.toml

    def on_placed(self, game: "Game", station: Station, item: Item) -> None:
        raise NotImplementedError

    def on_removed(self, game: "Game", station: Station, item: Item, progress: int) -> None:
        """The accepted tag left. progress is what the station measured."""

    def on_progress(self, game: "Game", station: Station, item: Item, value: int) -> None:
        pass

    def on_done(self, game: "Game", station: Station, item: Item) -> None:
        pass

    def tick(self, game: "Game", station: Station, dt: float) -> None:
        pass

    def display(self, game: "Game", station: Station) -> tuple[DisplayMode, int]:
        """What the LEDs show while nothing one-shot is playing."""
        return DisplayMode.IDLE, 0

    def describe(self, game: "Game", station: Station) -> dict:
        """Extra fields for the UI tile."""
        return {}


def _process(game: "Game", item: Item, station_name: str) -> Process | None:
    """The processing this station can do on the item right now, or None."""
    if item.is_plate:
        return None
    proc = game.level.ingredients[item.ingredient].processes.get(station_name)
    if proc is None or item.state != proc.from_state:
        return None
    return proc


class TaskStation(Behavior):
    """Cutting board, pan: the station measures input (presses, joystick
    steps) until the server's goal is reached."""

    def __init__(self, kind: StationKind, name: str, task: TaskKind):
        self.kind, self.name, self.task = kind, name, task

    def on_placed(self, game, station, item):
        proc = _process(game, item, self.name)
        if proc is None:
            game.reject(station, item, f"{item.label} ({item.state.value}) cannot go on a {self.name.replace('_', ' ')}")
            return
        progress = item.progress if item.progress_kind == self.name else 0
        game.accept(
            station, item, task=self.task, goal=proc.goal, progress=progress,
            param=PATTERN_IDS.get(proc.pattern, 0),
        )
        game.log(f"{station.label}: {item.label} on, {progress}/{proc.goal}")

    def on_progress(self, game, station, item, value):
        proc = _process(game, item, self.name)
        if proc is None:
            return
        item.progress = min(value, proc.goal)
        item.progress_kind = self.name

    def on_removed(self, game, station, item, progress):
        proc = _process(game, item, self.name)
        if proc is None:  # finished (state changed) or gone
            return
        item.progress = min(progress, proc.goal)
        item.progress_kind = self.name
        game.log(f"{station.label}: {item.label} picked up at {item.progress}/{proc.goal}")

    def on_done(self, game, station, item):
        proc = _process(game, item, self.name)
        if proc is None:
            return
        item.state = proc.to_state
        item.progress = 0
        item.progress_kind = None
        game.log(f"{station.label}: {item.label} is {item.state.value}", "ok")

    def describe(self, game, station):
        item = game.items.get(station.accepted) if station.accepted else None
        proc = _process(game, item, self.name) if item else None
        if item and proc:
            return {"progress": min(1.0, item.progress / proc.goal)}
        return {}


class PlateStation(Behavior):
    """A plate reader IS a plate: any food put on it goes onto that plate, at any
    time. The plate's own tag is touched to the delivery station to serve it."""

    kind = StationKind.PLATE
    name = "plate"

    def on_placed(self, game, station, item):
        if item.is_plate:
            game.reject(station, item, "plate tags are touched to the delivery station")
            return
        plate = game.plate_for(station)
        if plate is None:
            game.reject(station, item, "no plate tag is paired with this reader, recalibrate")
        elif plate.dirty:
            game.reject(station, item, "the plate is dirty, it needs washing")
        elif len(plate.contents) >= game.level.plate_capacity:
            game.reject(station, item, "plate is full")
        else:
            plate.contents.append(PlateEntry(item.uid, item.ingredient, item.state))
            item.state = ItemState.CONSUMED
            game.accept(station, item)
            game.flash(station, DisplayMode.CALIBRATED)
            game.log(f"{station.label}: {item.label} added to the plate", "ok")

    def display(self, game, station):
        plate = game.plate_for(station)
        if plate is None:
            return DisplayMode.IDLE, 0
        return (DisplayMode.PLATE_DIRTY if plate.dirty else DisplayMode.PLATE_CLEAN), 0

    def describe(self, game, station):
        plate = game.plate_for(station)
        if not plate:
            return {}
        if plate.dirty:
            return {"note": "dirty, needs washing"}
        return {"note": "on the plate: " + (", ".join(e.key for e in plate.contents) or "nothing")}


class Delivery(Behavior):
    """Serve a plate: touch its tag here and everything on its plate reader is
    handed in. A plate that matches an open order scores it; any other plate is
    dumped for a small penalty. Either way the food comes back as RAW after
    respawn_s and the plate is dirty. Any loose food placed here is thrown away
    the same way, which is how unwanted food is recycled."""

    kind = StationKind.DELIVERY
    name = "delivery"

    def on_placed(self, game, station, item):
        if not item.is_plate:
            game.trash(item)
            game.accept(station, item)
            game.flash(station, DisplayMode.CALIBRATED)
            return
        if item.dirty:
            game.reject(station, item, "the plate is dirty, it needs washing")
            return
        if not item.contents:
            game.reject(station, item, "the plate is empty")
            return
        order = game.match_order(item)
        game.accept(station, item)
        if order:
            game.complete_order(order, item)
        else:
            game.dump_plate(item)
        # Green for a delivery, red for a dump, on the delivery station and on the plate's own reader.
        result = DisplayMode.SUCCESS if order else DisplayMode.REJECT
        game.flash(station, result)
        reader = game.stations.get(item.home_mac) if item.home_mac else None
        if reader:
            game.flash(reader, result)


class Sink(Behavior):
    """Wash a dirty plate: touch its tag here and scrub until the station reports
    wash_s seconds of scrubbing (progress is in milliseconds, kept on the plate so
    it can be picked up and put back). A clean plate is the plate reader's again:
    its LEDs go back to green."""

    kind = StationKind.SINK
    name = "sink"

    def _goal(self, game) -> int:
        return round(game.level.wash_s * 1000)

    def on_placed(self, game, station, item):
        if not item.is_plate:
            game.reject(station, item, f"{item.label} is not a plate, only plates are washed here")
        elif not item.dirty:
            game.reject(station, item, "the plate is clean")
        else:
            progress = item.progress if item.progress_kind == self.name else 0
            game.accept(station, item, task=TaskKind.SCRUB, goal=self._goal(game), progress=progress)
            game.log(f"{station.label}: washing, {progress / 1000:.1f}/{game.level.wash_s:g} s")

    def _washing(self, item) -> bool:
        return item.is_plate and item.dirty

    def on_progress(self, game, station, item, value):
        if self._washing(item):
            item.progress, item.progress_kind = min(value, self._goal(game)), self.name

    def on_removed(self, game, station, item, progress):
        if self._washing(item):
            item.progress, item.progress_kind = min(progress, self._goal(game)), self.name
            game.log(f"{station.label}: plate picked up at {item.progress / 1000:.1f}/{game.level.wash_s:g} s")

    def on_done(self, game, station, item):
        if self._washing(item):
            item.dirty = False
            item.progress, item.progress_kind = 0, None
            game.log(f"{station.label}: plate is clean", "ok")

    def describe(self, game, station):
        item = game.items.get(station.accepted) if station.accepted else None
        if item and self._washing(item):
            return {"progress": min(1.0, item.progress / self._goal(game))}
        return {}


BEHAVIORS: dict[StationKind, Behavior] = {
    StationKind.CUTTING_BOARD: TaskStation(StationKind.CUTTING_BOARD, "cutting_board", TaskKind.PRESSES),
    StationKind.PAN: TaskStation(StationKind.PAN, "pan", TaskKind.JOYSTICK_PATTERN),
    StationKind.PLATE: PlateStation(),
    StationKind.DELIVERY: Delivery(),
    StationKind.FRYER: TaskStation(StationKind.FRYER, "deep_fryer", TaskKind.FRY),
    StationKind.SINK: Sink(),
}
