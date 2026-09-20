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
| `overcooked_pan/` | Frying pan station firmware (joystick moved in a circle / zigzag pattern) |
| `deep_fryer_station/` | Deep fryer station firmware (keep a hand's height, read by an HC-SR04, aligned with a roaming target shown on an OLED) |
| `overcooked_reader_station/` | Pot, plate and delivery stations: reader + LEDs only, one project with an environment per kind. A plate station is the plate itself (food goes on it); each plate also has a tag, touched to the delivery station to serve |
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
   (`pio run -t upload` in each folder; in `overcooked_reader_station` pick the kind with
   `-e pot`, `-e plate` or `-e delivery`).
2. `python -m overcooked --list-ports`, then `python -m overcooked --port /dev/cu.usbserial-XXXX`.
3. Open the page. Stations show up as they power on (green dot = heard within 3.5 s).

Every station and the bridge use the same RC522 and LED strip wiring, documented in
`shared/StationCore/src/StandardWiring.h`
(RC522: SDA→GPIO32, SCK→GPIO33, MOSI→GPIO25, MISO→GPIO26, RST→GPIO27, GND→GND, 3.3V→3V3; LEDs: DIN→GPIO13).
Extra inputs: cutting board limit switch on GPIO14 (`overcooked_cutting_board/src/main.cpp`), pan joystick
X→GPIO34, Y→GPIO35, powered from 3V3 (`overcooked_pan/src/main.cpp`), deep fryer HC-SR04 (TRIG→GPIO4,
ECHO→GPIO35 through a 5V→3.3V divider) and SSD1306 OLED (SDA→GPIO21, SCL→GPIO22)
(`deep_fryer_station/src/main.cpp`; these pins are my first guess, change them to match your build).

## Calibration (start of every game, or "Load last calibration")

1. Touch any tag to the **bridge's reader**: it becomes the calibration tag.
2. Touch that tag to **each station**. Each flashes green and is named
   ("Cutting board 1", "2", ... in touch order).
3. The page asks for each food in turn: touch a tag to the bridge's reader to say "this one is a
   tomato". Then it asks for each plate's tag ("Plate 1 tag"): that pairs the tag with that plate reader.
   A tag can only be enrolled once.

The result is saved to `overcooked_game/calibration.json`.

## The level

`overcooked_game/level.toml` holds everything you would tune: round length, how many of
each station, how many tags per ingredient, how much work each ingredient needs
(presses, joystick steps, cooking seconds), the menu and the scoring. The ingredients and
recipes in it are placeholders.

Rules worth knowing: progress stays on the server, so food can be picked up and put back,
even on another station of the same kind; cooking in a pot is timed and burns if left in;
a plate reader *is* a plate: put food on it whenever and it goes onto that plate, and touching that
plate's own tag to the delivery station serves what is on it. If it matches an open order that
scores; if not, the plate is dumped for a small penalty (`dump_penalty`). Either way the plate reader
flashes green (served) or red (dumped) and then glows dull brown while the plate is dirty (solid green when clean), the food comes back as raw after a few seconds, and the plate is
*dirty*: it takes no food and cannot be served until it is washed (a sink is planned; a new round or
Reset cleans every plate); loose food put on the delivery station is thrown away
(the way to recycle burnt food).

## Bridge serial protocol

ASCII lines, payloads in hex. Handy for poking a station from a serial monitor.

```
bridge -> laptop   BRIDGE <mac> <proto> | RX <mac> <type> <hex> | TXFAIL <mac> <type> | TAG <uid> | PONG | LOG <text>
laptop -> bridge   TX <mac|*> <type> <R|U> <hex> | PING | INFO       (R = acked and resent, U = fire and forget)
```

Message types and payload layouts are in `shared/OvercookedComm/src/OvercookedComm.h`, mirrored in
`overcooked_game/overcooked/protocol.py`; `tests/test_protocol.py` fails if the two drift apart.

## Adding a station type

Firmware: a new PlatformIO project next to `overcooked_pan` (copy its `platformio.ini`).
1. Subclass `station::StationTask` (see `overcooked_cutting_board/src/PressTask.h` or
   `overcooked_pan/src/JoystickPatternTask.h`) for the station's input. For a station with no
   input, pass no task (see `overcooked_reader_station`).
2. `main.cpp`: build a `station::StandardStation` with the right `oc::StationKind` and the task;
   call `begin()` in `setup()` and `update(millis())` in `loop()`.

Server (`overcooked_game/overcooked/`):
1. A brand new kind needs a `StationKind` in both `OvercookedComm.h` and `protocol.py`, and a
   `Behavior` in `kinds.py` (`BEHAVIORS`). A new joystick pattern needs an `oc::Pattern` value in
   `OvercookedComm.h` and the same id in `PATTERN_IDS` in `config.py` (a test checks they match), plus
   the movement itself in `overcooked_pan/src/PatternTracker.h`.
2. `level.toml`: add the station to `[stations]` and its work to the ingredients.

## Tests

```sh
cd overcooked_game && .venv/bin/pytest                  # protocol, calibration, gameplay, simulator, web
cd overcooked_cutting_board && pio test -e native       # tag presence logic
cd overcooked_pan && pio test -e native                 # joystick pattern logic
cd deep_fryer_station && pio test -e native              # hand/target overlap + progress scoring
pio run                                                 # in each firmware folder
```

## Known limits

- One tag per station at a time.
- Tag removal is detected after ~300 ms of misses (`PresenceReader`: 100 ms poll x 3). It needs tuning on
  real tags and the real RC522 wiring, which was not testable here.
- The pan's joystick reading (dead zone 0.6, low-pass filter) and the pattern logic are unit tested, but the
  feel of it is not: expect to tune `JOY_DEAD_ZONE` / `INVERT_X` / `INVERT_Y` and the goal in `level.toml`.
- Stations must be within ESP-NOW range of the bridge; a station that loses the bridge shows a slowly
  blinking red pixel and reconnects by itself.
- The deep fryer's `pulseIn()` HC-SR04 read briefly blocks `StationTask::update()` (up to ~6ms, capped to the
  station's NEAR_CM/FAR_CM range rather than the sensor's full ~5m timeout) each ping, which is a soft
  violation of "never block" in `StationTask.h`; fine at the current ping rate, worth an interrupt-driven
  echo read if it ever causes missed heartbeats. The target motion (drift/dart), the OLED tracker rendering
  and the physical hand-height feel are all untested, same as the pan's joystick feel.
