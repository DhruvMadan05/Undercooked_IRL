# Undercooked

**Overcooked, rebuilt with real appliances, cheap RFID tags, and a laptop that keeps score.**
A low-cost team-building game (roughly $130 of parts for a full kitchen) where the only way to
win is to work together.

Built by Team 2b2y (Ryan Ranjitkar, Luhai Tang, Dhruv Madan) for SASE Hack 2026.

## Why we built this

Overcooked is one of the best co-op games ever made, and it is still just a screen: everyone
crowded around a TV, mashing buttons, yelling at pixels. We wanted the yelling to be about a real
frying pan.

There is no affordable kit that turns a room into a physical co-op kitchen, so we built one. It is
meant to be cheap enough for any club, class, or team event. Nobody cooks alone: players call out
orders, hand off food, and cover for each other under a deadline.

- **Low cost:** about $130 in parts for the whole kitchen, plus a laptop you already own.
- **Easy to play:** real controls, nothing to learn. Press to chop, waggle to fry, scrub to wash, tap a tag to plate.
- **Easy to set up:** pair each station with one touch, plug in the bridge, and open the scoreboard on any device.
- **Real teamwork:** players talk, split jobs, and cover for each other.

## The concept

Up to eight cooking stations, cheap RFID tags standing in for ingredients, and a laptop that runs
the whole game. It is the chaos of the video game, except you are the one holding the joystick and
the frying pan, and your teammates are standing right next to you.

| In the video game | In our kitchen |
|---|---|
| Press a key to chop | Press a real limit switch on a real cutting board |
| Waggle a stick to fry | Waggle a joystick wired straight into an ESP32 |
| Watch a countdown on screen | Watch the same countdown in a browser everyone can see |
| Reset with Ctrl+R | Reset with one button: the laptop owns every bit of state |

## How a round plays

1. **Calibrate once.** Pair every station and every tag with one touch (see [Calibration](#calibration-start-of-every-game-or-load-last-calibration)).
2. **Orders appear.** Recipes spawn on the scoreboard with a deadline and a point value.
3. **Cook and prep.** Chop, fry, and scrub at the stations. Each one needs a different physical action.
4. **Plate and deliver.** Put food on a plate, then touch that plate's tag to the delivery station.
5. **Score and repeat.** Deliver before the clock runs out for a time bonus; miss it and you pay a penalty.

Progress lives on the server, not the station. Pick food up mid-task, put it down on a different
station of the same kind, and carry on exactly where you left off.

## The stations

Every station is the same build (an ESP32, an RC522 RFID reader, a 5-pixel WS2812B status strip)
plus, at most, one input:

| Station | Input | What it does |
|---|---|---|
| Cutting board | Limit switch presses | Raw -> chopped |
| Frying pan | Joystick, "Simon Says" gestures (circle, zigzag, hold, shake) with an OLED showing the next gesture | Raw -> cooked, burnt if you run out of time |
| Deep fryer | Hand height read by an HC-SR04, kept aligned with a roaming target on an OLED | Raw -> cooked |
| Sink | Joystick scrubbed in circles | Washes a dirty plate |
| Plate | None: the reader *is* the plate | Collects whatever is put on it |
| Delivery | Touch a plate's tag here | Matches an open order, scores it, and dirties the plate |

## Architecture

```
[station ESP32 x<=8] --ESP-NOW--> [bridge ESP32 + RC522] --USB serial--> [Python game server] --websocket--> [browser]
```

**All game state lives on the laptop.**

- **Tags are dumb:** cheap 13.56 MHz RFID chips, UID only, never written to.
- **Stations are dumb:** they report events (tag placed / removed, task progress) and draw whatever they are told.
- **The bridge is dumb:** a relay between ESP-NOW and USB serial, with its own RC522 used only for calibration.
- **The server owns everything else:** resetting the round, and every station with it, is one function call.

What that gets you:

- **Pure game engine.** A single class runs on events, a tick, and an injectable clock, with no serial or network inside, so tests drive it with fake time.
- **One behaviour per station kind.** A small class decides each kind's rules; the numbers live in a level file (`overcooked_game/level.toml`), not in code.
- **Calibration wizard.** Touch a tag to the bridge to name it, touch it to every station, then touch each ingredient once. The result is saved to disk and reloadable.
- **Live web UI.** The full game state is pushed over a websocket ten times a second to a plain-JavaScript scoreboard, with no build step.
- **Reliable radio.** ESP-NOW messages are acked, retried up to 5 times 200 ms apart, and de-duplicated by (MAC, sequence), so a lost ack never runs a handler twice.
- **Debuggable protocol.** The bridge speaks ASCII lines with hex payloads, so you can poke a station from a serial monitor.

## Engineering notes

- **One source of truth for the protocol.** A test parses the C++ header and fails the moment Python's protocol drifts from the firmware.
- **Zero-hardware development.** A simulated bridge speaks the real line protocol, so the whole game runs with no boards plugged in (`python -m overcooked --sim`).
- **Tested without an ESP32.** Python tests cover the protocol, calibration, gameplay, simulator, and web layer. Native C++ unit tests cover each station's pure logic.

## Rough budget

| Component | Unit cost | Qty | Notes |
|---|---|---|---|
| ESP32 dev board | ~$6 | 9 | 8 stations + 1 bridge |
| MFRC522 RFID reader | ~$2 | 9 | One per station + bridge |
| WS2812B strip (5px) | ~$1 | 8 | Status LEDs per station |
| Limit switch / joystick | ~$1.50 | 6 | Only input-driven stations |
| RFID tags (13.56 MHz) | ~$0.15 | 30 | Ingredients + plates |
| Wiring, perfboard, case | ~$4 | 9 | Rough per-station estimate |

About **$130** for a full 8-station kitchen. This is an estimate; swap in your real receipts.

## What's next

1. **More levels:** new kitchens, bigger orders, and tougher recipes, so teams have to coordinate harder each round.
2. **Better physical stations:** 3D-printed housings for the cutting board, pan, and sink instead of loose breadboards, still on a budget.
3. **More station types:** give the sink a real recipe, add new appliances, and show plate contents on their LEDs.
4. **Bigger teams:** more players, more stations, and a full table-sized kitchen for team events.

Come break it: jam the joystick, yank a tag mid-chop, unplug the bridge mid-round. The server is
built to shrug it off and pick up where you left it.

---

## Technical reference

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
| `sink_station/` | Sink station firmware: washes a dirty plate when a joystick is scrubbed in circles |
| `overcooked_reader_station/` | Plate and delivery stations: reader + LEDs only, one project with an environment per kind. A plate station is the plate itself (food goes on it); each plate also has a tag, touched to the delivery station to serve |
| `overcooked_server/` | Bridge firmware: ESP-NOW <-> USB serial, plus its own RC522 for calibration |
| `shared/OvercookedComm/` | ESP-NOW protocol (typed messages, acks, retries) |
| `shared/TagReader/` | RC522 wrapper with tag placed / removed detection |
| `shared/StationCore/` | Everything a station shares: LEDs, server session, task interface |
| `overcooked_game/` | The game server: rules, calibration, orders, web scoreboard, simulator |

All state lives on the laptop, so a reset is one button. The stations only report
what happened (tag placed / removed, task progress) and draw what they are told.

### Run the game without hardware

```sh
cd overcooked_game
python3 -m venv .venv && .venv/bin/pip install -e '.[dev]'
.venv/bin/python -m overcooked --sim        # then open http://127.0.0.1:8000
.venv/bin/pytest
```

The page has a simulator panel: virtual stations you can place tags on, and an
"Auto-calibrate" button.

### Run with the real bridge

1. Flash `overcooked_server` to the bridge ESP32 and each station firmware to its ESP32
   (`pio run -t upload` in each folder; in `overcooked_reader_station` pick the kind with
   `-e plate` or `-e delivery`).
2. `python -m overcooked --list-ports`, then `python -m overcooked --port /dev/cu.usbserial-XXXX`.
3. Open the page. Stations show up as they power on (green dot = heard within 3.5 s).

Every station and the bridge use the same RC522 and LED strip wiring, documented in
`shared/StationCore/src/StandardWiring.h`
(RC522: SDA→GPIO32, SCK→GPIO33, MOSI→GPIO25, MISO→GPIO26, RST→GPIO27, GND→GND, 3.3V→3V3; LEDs: DIN→GPIO13).
Extra inputs: cutting board limit switch on GPIO14 (`overcooked_cutting_board/src/main.cpp`), pan joystick
X→GPIO34, Y→GPIO35, powered from 3V3 (`overcooked_pan/src/main.cpp`), deep fryer HC-SR04 (TRIG→GPIO4,
ECHO→GPIO35 through a 5V→3.3V divider) and SSD1306 OLED (SDA→GPIO21, SCL→GPIO22)
(`deep_fryer_station/src/main.cpp`; these pins are my first guess, change them to match your build), sink
joystick X→GPIO34, Y→GPIO35, powered from 3V3 (`sink_station/src/main.cpp`).

### Calibration (start of every game, or "Load last calibration")

1. Touch any tag to the **bridge's reader**: it becomes the calibration tag.
2. Touch that tag to **each station**. Each flashes green and is named
   ("Cutting board 1", "2", ... in touch order).
3. The page asks for each food in turn: touch a tag to the bridge's reader to say "this one is a
   tomato". Then it asks for each plate's tag ("Plate 1 tag"): that pairs the tag with that plate reader.
   A tag can only be enrolled once.

The result is saved to `overcooked_game/calibration.json`.

### The level

`overcooked_game/level.toml` holds everything you would tune: round length, how many of
each station, how many tags per ingredient, how much work each ingredient needs
(presses, joystick steps, fryer goal), the menu and the scoring. The ingredients and
recipes in it are placeholders.

Rules worth knowing: progress stays on the server, so food can be picked up and put back,
even on another station of the same kind;
a plate reader *is* a plate: put food on it whenever and it goes onto that plate, and touching that
plate's own tag to the delivery station serves what is on it. If it matches an open order that
scores; if not, the plate is dumped for a small penalty (`dump_penalty`). Either way the plate reader
flashes green (served) or red (dumped) and then glows dull brown while the plate is dirty (solid green when clean), the food comes back as raw after a few seconds, and the plate is
*dirty*: it takes no food and cannot be served until it is washed (wash it at the sink: touch the plate's tag there and scrub the joystick in circles for `wash_s`
seconds, and the plate reader goes green again; a new round or Reset also cleans every plate); loose food put on the delivery station is thrown away.

### Bridge serial protocol

ASCII lines, payloads in hex. Handy for poking a station from a serial monitor.

```
bridge -> laptop   BRIDGE <mac> <proto> | RX <mac> <type> <hex> | TXFAIL <mac> <type> | TAG <uid> | PONG | LOG <text>
laptop -> bridge   TX <mac|*> <type> <R|U> <hex> | PING | INFO       (R = acked and resent, U = fire and forget)
```

Message types and payload layouts are in `shared/OvercookedComm/src/OvercookedComm.h`, mirrored in
`overcooked_game/overcooked/protocol.py`; `tests/test_protocol.py` fails if the two drift apart.

### Adding a station type

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

### Tests

```sh
cd overcooked_game && .venv/bin/pytest                  # protocol, calibration, gameplay, simulator, web
cd overcooked_cutting_board && pio test -e native       # tag presence logic
cd overcooked_pan && pio test -e native                 # joystick pattern logic
cd deep_fryer_station && pio test -e native              # hand/target overlap + progress scoring
cd sink_station && pio test -e native                    # scrubbing logic
pio run                                                 # in each firmware folder
```

### Known limits

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
