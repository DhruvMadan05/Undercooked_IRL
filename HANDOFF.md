# Handoff: Overcooked stations

For an engineer or coding agent picking this project up cold. `README.md` is the user-facing overview;
this file is what you need to keep working: how it fits together, why, what is verified, what is not.

## What this is
A physical Overcooked game. Food is a cheap RFID tag (UID only, **never written to**). Each cooking station
is an ESP32 with an RC522 reader, a WS2812B LED strip and station-specific controls. A laptop runs the
game and shows the scoreboard/timer in a browser. Owner: Dhruv (GitHub `DhruvMadan05`), hardware-hobby
project, iterating with real boards; up to 8 stations.

```
[station ESP32 <=8] --ESP-NOW--> [bridge ESP32 + RC522] --USB serial--> [Python game server] --websocket--> [browser]
```

Core principle: **all game state lives on the laptop.** Tags are dumb, stations report events and draw
what they are told, the bridge is a dumb relay. Reset = one function call.

## Repo map (git root is `Overcooked/`, branch `main`, working tree clean at time of writing)
| Path | What |
|---|---|
| `shared/OvercookedComm/` | ESP-NOW protocol: typed messages, per-peer acks/retries. **Wire format source of truth.** |
| `shared/TagReader/` | `PresenceTracker` (pure logic, native-tested) + `PresenceReader` (RC522, WUPA polling) |
| `shared/StationCore/` | `Display` (LEDs), `Session` (Hello/Welcome/heartbeat), `Station` (glue), `StationTask` interface, `StandardWiring.h` (pin map), `StandardStation.h` (one-object station) |
| `overcooked_cutting_board/` | Station: `PressTask` counts limit-switch presses (GPIO14). Also has the `native` unit-test env |
| `overcooked_pan/` | Station: `JoystickPatternTask` (analog X/Y GPIO34/35), pure `PatternTracker` (circle / zigzag), native tests |
| `deep_fryer_station/` | Station: `FryTask` (HC-SR04 hand height GPIO4/35, SSD1306 OLED I2C GPIO21/22), pure `fryer::ProgressTracker`/`overlaps()` (target overlap + fill/drain scoring), native tests |
| `sink_station/` | Station: `ScrubTask` (analog joystick GPIO34/35, same pins as the pan), pure `scrub::ScrubTracker` (counts milliseconds of active circling), native tests |
| `overcooked_reader_station/` | Plate / delivery: same firmware, env picks the `StationKind` (`-e plate|delivery`, one at a time) |
| `overcooked_server/` | **The bridge** (name is historical): ESP-NOW <-> USB serial + its own RC522 for calibration |
| `overcooked_game/` | Python game server (`overcooked/` package), `level.toml`, browser UI in `overcooked/static/`, pytest suite |
| `Overcooked_test/` | Early scratch project from before this work. Ignore it. |

## Protocol (v2)
- ESP-NOW packet = `{magic 0xC7, type, seq, flags}` + packed little-endian payload (<= 32 bytes). `flags` bit0 = reliable.
- Reliable messages: per-destination stop-and-wait queue, 5 attempts x 200 ms, receiver acks + dedupes by (mac, seq).
  Unreliable: sent immediately (progress, display refresh, heartbeat). The sender of a `TX` line can override
  reliability per message (used for one-shot LED flashes, which must not be lost).
- Types: `Hello, Heartbeat, TagPlaced, TagRemoved(+progress), TaskProgress, TaskDone` (station -> server),
  `Welcome, Accept(task, goal, progress, param), Reject, SetDisplay(mode, level)` (server -> station).
- Station link: broadcasts `Hello` until `Welcome`, then unicasts to the bridge MAC with a 1 Hz `Heartbeat`;
  3.5 s of silence -> back to `Hello` (station shows a slow red blink). A `Welcome` while already connected
  means the server restarted, so the station re-announces the tag on it. The server sends `Welcome` on the first
  heartbeat from a MAC it does not know.
- Bridge <-> laptop: ASCII lines, hex payloads. Bridge->host `BRIDGE|RX|TXFAIL|TAG|PONG|LOG`; host->bridge
  `TX <mac|*> <type> <R|U> <hex>|PING|INFO`. Debuggable from a serial monitor. **On the bridge, `Serial` is the
  data channel: never `Serial.print` debug text there, use `logLine()`.**
- Layouts are mirrored in `shared/OvercookedComm/src/OvercookedComm.h` and `overcooked_game/overcooked/protocol.py`.
  `tests/test_protocol.py` regex-parses the C++ header and fails on drift (enum values, struct sizes,
  default reliability, protocol version, joystick pattern ids). To add a message, follow the checklist in the header.

## Game model (Python, `overcooked_game/overcooked/`)
- `engine.py` `Game`: pure logic. Fed events (`handle`), a clock (`tick`, injectable `now`) and UI actions
  (`action("start_game")` etc.); sends via an injected callable. No serial/network/wall clock inside it, so tests run on a fake clock.
- `kinds.py`: one `Behavior` per station kind (`TaskStation` for cutting board + pan + deep fryer, `Sink`, `PlateStation`, `Delivery`).
  Rules in code, numbers/menu in `level.toml` (parsed and validated by `config.py`).
- `model.py`: `Item` (one physical tag: ingredient or plate; state raw/chopped/cooked/consumed; progress;
  plate `contents` and `home_mac`), `Station`, `Order`, `Phase`.
- Phases: `cal_master -> cal_stations -> cal_food -> ready -> countdown -> playing -> ended`.
- **Calibration**: touch any tag to the *bridge's* reader (becomes the calibration tag) -> touch it to each station
  (named in touch order, "Cutting board 1"...) -> touch each food tag to the bridge reader while the wizard names
  the ingredient -> touch each plate tag ("Plate N tag"), which pairs it with the Nth calibrated plate reader.
  Saved to `overcooked_game/calibration.json` (git-ignored), "Load last calibration" in the UI.
- **Progress is server-side**: `Accept` carries saved progress, `TagRemoved` reports where the station stopped, so
  food can be picked up and put back, even on another station of the same kind.
- **Deep fryer**: like the pan, a `TaskStation` (`kind="deep_fryer"`, `TaskKind.FRY`) - the firmware runs its own
  minigame (align hand height with a roaming target) and reports progress 0..goal same as presses/pattern
  steps, except progress can fall back down (a miss drains it) as well as rise, which `TaskProgress`/resume
  already supported without changes. No new protocol message was needed.
- **Plates (latest change)**: a plate reader *is* a plate. Any food put on it is added to that plate immediately
  (`game.plate_for(station)` finds the plate whose `home_mac` is that reader); plate tags are rejected there.
  Touching a plate's tag at the delivery station serves what is on its reader. No `[plates]` in `level.toml`
  any more (it raises a `ConfigError`); plate count = `[stations] plate = N`.
- **Dump + dirty plates**: at delivery a plate with no matching order is dumped (`Game.dump_plate`, `dump_penalty`
  points, floored at 0); a matching one is delivered. Both go through `Game._use_up_plate`: food respawns raw after
  `respawn_s`, `Item.dirty = True`. A dirty plate is rejected by its plate reader and by delivery (no penalty), and
  the plate tile shows "dirty, needs washing". `Item.reset()` (new round / Reset) clears it. Dirty plates are washed at the
  **sink** (`kinds.Sink`, `StationKind::Sink`, `TaskKind::Scrub`): touch the plate's tag there, scrub the joystick, and
  after `wash_s` seconds (`level.toml`, max 60) `dirty` goes back to False and the plate reader's LEDs go green. Goal and
  progress are milliseconds of active scrubbing, kept on the plate (`progress_kind = "sink"`) so a half-washed plate resumes.
  The sink refuses clean plates, food and unknown tags. Without a sink in `[stations]` a plate is single-use per round.
  The plate reader flashes SUCCESS (green x5) / REJECT (red x3) via `game.flash`, and the delivery station flashes the
  same result. Between flashes a plate reader shows its plate's state during a round: `DisplayMode.PlateClean` (solid
  green) or `PlateDirty` (dull brown), from `PlateStation.display`. Colours are in `shared/StationCore/src/Display.cpp`.
  These two modes (9, 10) are new; wire layout unchanged, so `kProtocolVersion` stays 2 and older firmware just draws
  nothing for them. Reflash the plate reader(s) (`-e plate`) to get them.
- The pot station was removed (station kind 2 and display modes 2-4 are retired but their wire ids are left
  unused, so the other stations keep their ids and need no reflash). Delivered/thrown-away food respawns as RAW after `respawn_s`; loose food on the delivery station is trashed
  (the bin). Orders spawn from recipes, expire with a penalty, deliveries score with a time bonus.
- `sim.py`: `SimBridge` speaks the real bridge line protocol with virtual stations that mimic the firmware, so the
  whole stack (link codec, Game, UI) runs with no hardware: `python -m overcooked --sim`.
- `web.py`: FastAPI, websocket `/ws` pushes the full state at 10 Hz and takes `{"action": ...}` (`sim_*` actions go to the simulator).
  UI is vanilla JS with no build step; `setHTML()` skips identical HTML so buttons survive re-renders.

## Build, test, run
```sh
# Python (venv already at overcooked_game/.venv)
cd overcooked_game && .venv/bin/python -m pytest -q          # 115 tests, ~0.5 s
.venv/bin/python -m overcooked --sim                          # http://127.0.0.1:8000
.venv/bin/python -m overcooked --list-ports
.venv/bin/python -m overcooked --port /dev/cu.usbserial-XXXX  # real bridge

# Firmware. `pio` is NOT on PATH; it lives in ~/.platformio/penv/bin
PIO=~/.platformio/penv/bin/pio
cd overcooked_cutting_board && $PIO run && $PIO test -e native   # 6 tests
cd overcooked_pan           && $PIO run && $PIO test -e native   # 8 tests
cd deep_fryer_station       && $PIO run && $PIO test -e native   # 8 tests
cd sink_station             && $PIO run && $PIO test -e native   # 8 tests
cd overcooked_server        && $PIO run
cd overcooked_reader_station && $PIO run -e delivery             # one env at a time; -t upload to flash
```
Every firmware project sets `default_envs` so a bare `pio run` works; in `overcooked_reader_station` a bare
`pio run -t upload` would flash both kinds in a row, so always pass `-e`.

## Gotchas
- **Do not let PlatformIO move to Arduino-ESP32 core 3.x.** `platform = espressif32` is unpinned; installed is platform 7.0.1 /
  Arduino core 2.0.17. `esp_now_register_recv_cb` uses the 2.x signature `(const uint8_t *mac, ...)` in
  `OvercookedComm.cpp`; core 3.x changes it to `esp_now_recv_info_t*` and the build breaks.
- macOS `sed -i` needs `sed -i ''`. Prefer the Edit tool.
- `.overlay` in the CSS overrides the `hidden` attribute unless `[hidden]{display:none !important}` is kept (it is).
- Opening the serial port resets most ESP32 dev boards (DTR/RTS), so the bridge reboots on connect and prints
  `BRIDGE ...` again; the parser ignores boot noise. Any bridge event marks it connected.
- RC522 presence: every poll sends WUPA (wake-up), not REQA, so halted tags still answer and "no answer" means removed
  (3 misses x 100 ms). One tag per reader at a time; the firmware tracks a single UID.
- Joystick modules must be powered from 3V3, not 5V (ESP32 ADC pins). Use ADC1 pins only (ADC2 dies when WiFi is on).
- Something typed stray text into `overcooked_server/platformio.ini` once (broke the build). If a `pio` config error
  looks like prose, check the first line.
- On this machine (Apple clang, `platformio/native` 1.2.1) `pio test -e native` defaults to a pre-C++11 standard,
  which fails on `constexpr`/`enum class` in any pure-logic header (`PatternTracker.h`, `FryTracker.h`, ...).
  `deep_fryer_station/platformio.ini` works around it with `-std=gnu++17` in the native env's `build_flags`;
  `overcooked_cutting_board` and `overcooked_pan` do not have that flag yet and will hit the same failure until
  it is added there too.
- `overcooked_game/calibration.json` currently holds a **real-hardware calibration made before plate pairing existed**
  (its plate has no `station` key). Loading it leaves the plate unpaired, so the plate reader rejects food with
  "no plate tag is paired with this reader". Recalibrate. Do not delete the file without asking; it is the user's.

## Verified vs not verified
- Verified in this environment: everything compiles for all firmware targets (including the new
  `deep_fryer_station`); pytest (115 tests) and the cutting board / pan / deep fryer / sink native test suites pass
  (native needs `-std=gnu++17`, see Gotchas); a full simulated round (calibrate, cut with resume across boards,
  pan, plate, delivery) runs through the real line protocol and browser UI (screenshotted in
  headless Chrome); the serial transport works over a pty. The deep fryer's server-side behaviour (accept,
  progress resume on pickup, reject wrong food, `on_done` -> cooked) is unit tested the same way as the pan.
- Not verified by the author of this file: anything on real hardware. The user has since run real boards (the saved
  `calibration.json` has real MACs), but no results were reported back. Specifically untested: RC522 removal detection
  tuning, ESP-NOW range with several stations, LED behaviour, joystick feel (`JOY_DEAD_ZONE`, `INVERT_X/Y`),
  the bridge over a real USB port, pan/plate/delivery firmware end to end, the sink's joystick scrubbing feel (`kDeadzoneRadius` / `kMinDeltaAngle` in `ScrubTracker.h`, `wash_s`), and the entire deep fryer minigame
  (HC-SR04 reading, target motion feel, OLED wiring/address) - it only ran as a hand-tested standalone sketch
  before being integrated into StationCore here.
- The joystick pins (GPIO34/35), the deep fryer's HC-SR04/OLED pins (GPIO4/35/21/22) and hand-height range
  (`NEAR_CM`/`FAR_CM`/`CATCH_ZONE_FRAC`), and the menu in `level.toml` (tomato, patty, potato, ...
  recipes, points) are placeholders/guesses, easy to change.

## Sensible next steps
1. Get hardware feedback and tune: removal miss count in `PresenceReader` (defaults 100 ms x 3), joystick dead zone,
   goals and cook times in `level.toml`.
2. If plate readers should show what is on the plate (they show clean / dirty today), add a `DisplayMode` (needs `OvercookedComm.h` + `protocol.py` + a `Display.cpp` case).
3. More than one tag on a reader (e.g. several foods on a plate at once) needs multi-tag reading in `PresenceReader`
   (RC522 anticollision) and a set-based `TagPlaced`/`TagRemoved`; deliberately not done.
4. Real-time bridge health: only a hung-but-connected bridge is undetected today (no periodic ping).
5. Add `-std=gnu++17` to `overcooked_cutting_board` and `overcooked_pan`'s native envs (see Gotchas) so
   `pio test -e native` works there again on this toolchain, matching `deep_fryer_station/platformio.ini`.
6. The deep fryer's `pulseIn()` HC-SR04 read blocks `update()` for up to ~6ms per ping (see README "Known
   limits"); switch to an interrupt/timer-driven echo read if that is ever measured to cause missed heartbeats.

## Working with this user
- They want a plan before big changes (they asked for one and approved it). Ask only when a decision is truly theirs;
  their answers can be terse, so restate your interpretation when you act on one.
- Do not commit or push unless asked. Commits go on `main` now; the user commits themselves.
- Code style here: comments explain why, not what; match the surrounding density; no dead code or speculative options.
