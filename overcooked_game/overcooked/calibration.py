"""The calibration checklist and its saved file.

Sequence (driven by Game): touch one tag to the bridge reader (it becomes the
calibration tag) -> touch that tag to every station -> touch every food and
plate tag to the bridge reader, saying what each one is.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from .config import Level
from .protocol import KIND_NAMES, StationKind


@dataclass
class Slot:
    """One station the level expects ("cutting_board" #1)."""
    kind: str
    index: int
    mac: str | None = None


@dataclass
class FoodStep:
    """One thing to enrol: N tags of an ingredient, or the tag of one plate."""
    name: str  # ingredient name or "plate"
    total: int
    plate_index: int | None = None  # set for a plate: which plate reader it belongs to
    uids: list[bytes] = field(default_factory=list)

    @property
    def done(self) -> bool:
        return len(self.uids) >= self.total

    @property
    def label(self) -> str:
        return self.name if self.plate_index is None else f"{slot_name('plate', self.plate_index)} tag"


class Checklist:
    def __init__(self, level: Level):
        self.slots = [
            Slot(kind, i) for kind, count in level.stations.items() for i in range(count)
        ]
        self.steps = [FoodStep(name, ing.count) for name, ing in level.ingredients.items()]
        # One tag per plate reader, in the order the readers were calibrated.
        self.steps += [FoodStep("plate", 1, plate_index=i) for i in range(level.plates)]
        self.step_index = 0

    # stations
    def slot_of(self, mac: str) -> Slot | None:
        return next((s for s in self.slots if s.mac == mac), None)

    def slot_for(self, kind: str, index: int) -> Slot | None:
        return next((s for s in self.slots if s.kind == kind and s.index == index), None)

    def free_slot(self, kind: str) -> Slot | None:
        return next((s for s in self.slots if s.kind == kind and s.mac is None), None)

    @property
    def stations_done(self) -> bool:
        return all(s.mac for s in self.slots)

    # food
    @property
    def current_step(self) -> FoodStep | None:
        return self.steps[self.step_index] if self.step_index < len(self.steps) else None

    def advance(self) -> None:
        """Move to the next ingredient (skipping the rest of the current one)."""
        if self.step_index < len(self.steps):
            self.step_index += 1

    @property
    def food_done(self) -> bool:
        return self.step_index >= len(self.steps)

    def to_dict(self) -> dict:
        return {
            "slots": [{"kind": s.kind, "index": s.index, "mac": s.mac, "unassigned": s.mac is None} for s in self.slots],
            "steps": [
                {"name": st.name, "label": st.label, "total": st.total, "count": len(st.uids),
                 "current": i == self.step_index}
                for i, st in enumerate(self.steps)
            ],
        }


def slot_name(kind: str, index: int) -> str:
    return f"{kind.replace('_', ' ').capitalize()} {index + 1}"


# ---- Saved calibration ----------------------------------------------------------

def save(path: Path, master: bytes, stations: dict[str, tuple[StationKind, str]],
         items: dict[bytes, tuple[str | None, str | None]]) -> None:
    """stations: mac -> (kind, name). items: uid -> (ingredient name or None for a plate,
    the plate reader's mac for a plate)."""
    data = {
        "master": master.hex(),
        "stations": [{"mac": mac, "kind": KIND_NAMES[kind], "name": name} for mac, (kind, name) in stations.items()],
        "items": [{"uid": uid.hex(), "ingredient": ing, "station": home} for uid, (ing, home) in items.items()],
    }
    path.write_text(json.dumps(data, indent=2))


def load(path: Path) -> dict | None:
    """The saved calibration as plain data, or None if there is none / it is unreadable."""
    try:
        data = json.loads(path.read_text())
        return {
            "master": bytes.fromhex(data["master"]),
            "stations": [(s["mac"], s["kind"], s["name"]) for s in data["stations"]],
            "items": [(bytes.fromhex(i["uid"]), i["ingredient"], i.get("station")) for i in data["items"]],
        }
    except (OSError, ValueError, KeyError, TypeError):
        return None
