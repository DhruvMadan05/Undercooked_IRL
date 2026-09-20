# Overcooked stations

A physical Overcooked: food is a cheap RFID tag (UID only, never written), each
cooking station is an ESP32 with an RC522 reader plus its own controls, and a
laptop runs the game and shows the scoreboard.

```
[station ESP32 x<=8] --ESP-NOW--> [bridge ESP32 + RC522] --USB serial--> [Python game server] --websocket--> [browser]
```

| Folder | What |
|---|---|
| `overcooked_cutting_board/` | Cutting board station firmware (counts limit-switch presses) |
| `overcooked_server/` | Bridge firmware: ESP-NOW <-> USB serial, plus its own RC522 for calibration |
| `shared/OvercookedComm/` | ESP-NOW protocol (typed messages, acks, retries) |
| `shared/TagReader/` | RC522 wrapper with tag placed / removed detection |
| `shared/StationCore/` | Everything a station shares: LEDs, server session, task interface |
| `overcooked_game/` | The game server: rules, calibration, orders, web scoreboard, simulator |

All state lives on the laptop, so a reset is one button. The stations only report
what happened (tag placed / removed, task progress) and draw what they are told.

## Run the game without hardware

```sh
cd overcooked_game
python3 -m venv .venv && .venv/bin/pip install -e '.[dev]'
.venv/bin/python -m overcooked --sim        # then open http://127.0.0.1:8000
.venv/bin/pytest
```

The page has a simulator panel: virtual stations you can place tags on, and an
"Auto-calibrate" button.

## Run with the real bridge

1. Flash `overcooked_server` to the bridge ESP32 and each station firmware to its ESP32
   (`pio run -t upload` in each folder).
2. `python -m overcooked --list-ports`, then `python -m overcooked --port /dev/cu.usbserial-XXXX`.
3. Open the page. Stations show up as they power on (green dot = heard within 3.5 s).

The bridge needs an RC522 wired like the cutting board:
SDA→GPIO32, SCK→GPIO33, MOSI→GPIO25, MISO→GPIO26, RST→GPIO27, GND→GND, 3.3V→3V3.

## Calibration (start of every game, or "Load last calibration")

1. Touch any tag to the **bridge's reader**: it becomes the calibration tag.
2. Touch that tag to **each station**. Each flashes green and is named
   ("Cutting board 1", "2", ... in touch order).
3. The page asks for each food and plate in turn: touch a tag to the bridge's reader to
   say "this one is a tomato". A tag can only be enrolled once.

The result is saved to `overcooked_game/calibration.json`.

## The level

`overcooked_game/level.toml` holds everything you would tune: round length, how many of
each station, how many tags per ingredient, how much work each ingredient needs
(presses, joystick steps, cooking seconds), the menu and the scoring. The ingredients and
recipes in it are placeholders.

Rules worth knowing: progress stays on the server, so food can be picked up and put back,
even on another station of the same kind; cooking in a pot is timed and burns if left in;
scan a plate at the plate station, then scan food to add it; delivered food comes back
as raw after a few seconds; loose food put on the delivery station is thrown away
(the way to recycle burnt food).

## Bridge serial protocol

ASCII lines, payloads in hex. Handy for poking a station from a serial monitor.

```
bridge -> laptop   BRIDGE <mac> <proto> | RX <mac> <type> <hex> | TXFAIL <mac> <type> | TAG <uid> | PONG | LOG <text>
laptop -> bridge   TX <mac|*> <type> <R|U> <hex> | PING | INFO       (R = acked and resent, U = fire and forget)
```

Message types and payload layouts are in `shared/OvercookedComm/src/OvercookedComm.h`, mirrored in
`overcooked_game/overcooked/protocol.py`; `tests/test_protocol.py` fails if the two drift apart.

## Adding a station type (e.g. the pan)

Firmware, a new PlatformIO project next to `overcooked_cutting_board` (copy its `platformio.ini`):
1. Subclass `station::StationTask` (see `overcooked_cutting_board/src/PressTask.h`) for the station's
   input: joystick pattern, stirring, ... For a station with no input, pass no task.
2. `main.cpp`: build the reader, LED strip and task, construct `station::Station` with the right
   `oc::StationKind`, call `begin()` and `update(millis())`.

Server (`overcooked_game/overcooked/`):
1. `kinds.py`: the pan already exists (`TaskStation`, joystick pattern, `Accept.param` = pattern id from
   `PATTERN_IDS` in `config.py`; keep those ids in step with the firmware). A brand new kind needs a
   `StationKind` in both `OvercookedComm.h` and `protocol.py`, and a `Behavior` in `BEHAVIORS`.
2. `level.toml`: add the station to `[stations]` and its work to the ingredients.

The pan, pot, plate and delivery *firmware* is not written yet; the server side of all of them is, and is
covered by the simulator and tests.

## Tests

```sh
cd overcooked_game && .venv/bin/pytest                  # protocol, calibration, gameplay, simulator, web
cd overcooked_cutting_board && pio test -e native       # tag presence logic
pio run                                                 # in overcooked_cutting_board and overcooked_server
```

## Known limits

- One tag per station at a time.
- Tag removal is detected after ~300 ms of misses (`PresenceReader`: 100 ms poll x 3). It needs tuning on
  real tags and the real RC522 wiring, which was not testable here.
- Stations must be within ESP-NOW range of the bridge; a station that loses the bridge shows a slowly
  blinking red pixel and reconnects by itself.
