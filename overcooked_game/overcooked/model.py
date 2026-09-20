"""Plain data the game keeps about tags and stations."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from .protocol import DisplayMode, StationKind


class Phase(str, Enum):
    CAL_MASTER = "cal_master"      # touch the calibration tag to the bridge reader
    CAL_STATIONS = "cal_stations"  # touch it to every station
    CAL_FOOD = "cal_food"          # touch every food / plate tag to the bridge reader
    READY = "ready"
    COUNTDOWN = "countdown"
    PLAYING = "playing"
    ENDED = "ended"


class ItemState(str, Enum):
    RAW = "raw"
    CHOPPED = "chopped"
    COOKED = "cooked"
    BURNT = "burnt"
    CONSUMED = "consumed"  # on a delivered plate / trashed, back to RAW after respawn_s


@dataclass
class PlateEntry:
    uid: bytes
    ingredient: str
    state: ItemState

    @property
    def key(self) -> str:
        return f"{self.ingredient}:{self.state.value}"


@dataclass
class Item:
    """One physical tag: a piece of food or a plate."""
    uid: bytes
    ingredient: str | None  # None = plate
    state: ItemState = ItemState.RAW
    progress: int = 0                # task units, or milliseconds in a pot
    progress_kind: str | None = None  # station kind the progress belongs to
    respawn_at: float | None = None
    contents: list[PlateEntry] = field(default_factory=list)  # plates only
    home_mac: str | None = None  # plates only: the plate reader (station) this tag belongs to

    @property
    def is_plate(self) -> bool:
        return self.ingredient is None

    @property
    def label(self) -> str:
        return "plate" if self.is_plate else self.ingredient

    def reset(self) -> None:
        self.state = ItemState.RAW
        self.progress = 0
        self.progress_kind = None
        self.respawn_at = None
        self.contents.clear()


@dataclass
class Station:
    mac: str
    kind: StationKind
    name: str | None = None      # "Cutting board 1", set by calibration
    last_seen: float = 0.0
    online: bool = False
    item: bytes | None = None      # tag on the reader right now
    accepted: bytes | None = None  # tag the server accepted (its task runs on this)
    sent_display: tuple[DisplayMode, int] | None = None
    data: dict = field(default_factory=dict)  # per-kind runtime state

    @property
    def label(self) -> str:
        return self.name or f"{self.kind.name.replace('_', ' ').title()} ..{self.mac[-4:]}"


@dataclass
class Order:
    id: int
    recipe: str
    needs: tuple[str, ...]
    points: int
    created: float
    deadline: float
