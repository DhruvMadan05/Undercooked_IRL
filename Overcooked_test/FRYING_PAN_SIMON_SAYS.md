# Frying Pan Simon Says Game - ESP32 Project

## Project Overview
An interactive cooking-themed Simon Says game built with an ESP32 microcontroller. Players follow increasingly complex sequences of frying pan movements on a joystick controller. The game features visual feedback on an OLED display, LED indicators, and sound effects to create an engaging arcade-style experience.

## Hardware Requirements
- **ESP32 Development Board**
- **Analog Joystick Module** (2-axis with click button)
- **2x SSD1306 OLED Displays** (both 128x32, I2C) - a 0.96" "move" screen for the move animation and a 0.91" "bar" screen for the timer
- **Buzzer/Speaker** (connected to PWM pin) - *planned, not yet implemented in code*
- **2x LEDs** (green for correct moves, red for wrong moves) - *planned, not yet implemented in code*
- **Jumper Wires & Breadboard**

## Pin Configuration
Authoritative source is `src/FryingPanPins.h`:
```
JOYSTICK_X       -> GPIO 39 (ADC0, input-only)
JOYSTICK_Y       -> GPIO 36 (ADC3, input-only)
JOYSTICK_CLICK   -> GPIO 14

MOVE OLED (0.96", animation)   -- I2C bus 0 (Wire)
SDA              -> GPIO 32
SCL              -> GPIO 33

BAR OLED (0.91", timer bar)    -- I2C bus 1 (Wire1)
SDA              -> GPIO 25
SCL              -> GPIO 26

Both panels use address 0x3C.
```

**Why two I2C buses:** these modules are almost always hardwired to address 0x3C, so two of them can't share one bus unless you change one module's address jumper. The ESP32 has a second I2C peripheral (`Wire1`), so each panel gets its own bus and neither module needs modifying. `fryingPanSetup()` brings up both buses itself and passes `periphBegin = false` to `Adafruit_SSD1306::begin()`, because the library's own `wire->begin()` takes no pin arguments and would otherwise re-init the bus.

## Required Libraries (see `platformio.ini`)
- `adafruit/Adafruit GFX Library`
- `adafruit/Adafruit SSD1306`
- `lionrocker/Better Joystick` - wraps the raw `analogRead`/`digitalRead` calls for the joystick into `x()`, `y()`, `isPressed()`.

**Note on Better Joystick:** it assumes a 10-bit ADC (0-1023, center 512). The ESP32's ADC defaults to 12-bit, so `setup()` explicitly calls `analogReadResolution(10)` to match. The library's built-in `facingDirection()`/dead-center radius (a hardcoded 30 units) isn't used because it isn't configurable; instead `main.cpp` reads the raw `x()`/`y()` values and applies its own tunable `JOYSTICK_DEADZONE`. All gesture recognition (hold/shake/rotate/press/flick) is hand-written on top of the library, since it only reads the stick and button.

## Game Mechanics

### The 9 Moves
The game randomizes from a pool of 9 realistic frying pan movements (never repeating the same move twice in a row):

1. **UP (^)** - Push joystick upward
2. **DOWN (v)** - Push joystick downward
3. **LEFT (<)** - Push joystick left
4. **RIGHT (>)** - Push joystick right
5. **HOLD STEADY** - Keep joystick perfectly centered for 500ms (no movement)
6. **RAPID SHAKE** - Quick alternation between left-right OR up-down (3 alternating edges, all within 1 second)
7. **ROTATE** - One full circular lap around the four directions (4 direction changes)
8. **PRESS DOWN** - Click button and hold for 300ms+
9. **FLICK UP** - Click+hold button, then flick joystick upward

### Gameplay Flow
1. **Menu Screen** - Shows title, prompts player to press the joystick to start
2. **Cook Timer** - A 30s timer starts counting down in real time the moment cooking starts
3. **Player Input** - A single random move is shown at a time (as a big symbol, or a small animation for the gesture moves - see below); performing it correctly speeds up the timer and rolls a new move. A wrong move does nothing bad to the timer, only breaks the bonus chain (see below) and re-prompts the same move with a small on-screen mark
4. **Bonus Chain** - Each correct move in a row removes more time than the last: 0.5s, 1.0s, 1.5s, ... capped at 3.0s per move. A wrong move resets the chain back to 0.5s
5. **Cooked / Take It Off** - When the timer hits 0, the pan enters a 10s grace period ("COOKED! Take it off!"). Clicking the stick in time finishes the dish (`STATE_DONE`); letting the grace period run out sets it on fire
6. **Catching Fire** - Fire also strikes immediately, any time during cooking, if the player racks up 10 wrong moves total (they don't have to be consecutive - a hidden counter tracks it across the whole cook). Either fire trigger shows an animated flame screen flashing "FIRE FIRE FIRE FIRE"; a click starts a fresh cook
7. **Done** - Successfully taking the dish off in time shows the final score = total correct moves made ("Moves hit")

## Feedback Systems

### Visual Feedback
Each panel has exactly one job, so neither is cluttered:

- **Move screen (0.96")** - The current move's animation, plus a compact progress bar of its own along the bottom edge. This screen alone is enough to play the whole game.
- **Bar screen (0.91")** - Optional. Nothing but one big progress bar filling the panel, with the remaining time printed across it at double size in INVERSE, so the digits stay legible whether they land on the filled or empty part of the bar. It shows the cook countdown during play and the burn-grace countdown after the dish is cooked. If it isn't wired up (or fails `begin()`), `fryingPanSetup()` prints a note to serial and the game runs fine on the move screen's own bar alone - nothing hangs waiting for it.
- **Move animations** - Every one of the 9 moves is an animated "joystick ball," each holding its pose for half a second at a time so the motion is easy to read rather than a fast blur: the ball rests then holds out toward the edge for the 4 directional moves, pulses slowly in place for HOLD, orbits the socket for ROTATE, and dips down and holds for PRESS. FLICK gets a longer 3-part cycle - pressed down with the click line, then snapped up past the normal push distance with shrinking ghost balls trailing along the path it travelled, so it reads as a fast upward snap instead of the ball teleporting. SHAKE is the one animation left brisk and back-and-forth, since that repeated motion is literally what a shake looks like - it's what all the others used to look like by accident, which was confusing.
- **Correct mark** - A small filled circle flashes in the move screen's top-left corner for 250ms on a correct move
- **Miss mark** - A small filled square flashes in the move screen's top-right corner for 250ms on a wrong move - both are deliberately small/non-blocking rather than full-screen banners, and opposite shapes/corners so they're easy to tell apart at a glance
- **Fire screen** - Flickering flame triangles fill the move screen while the bar screen flashes "FIRE FIRE / FIRE FIRE" at double size
- **Green LED** - Flashes 150ms on a correct move
- **Red LED** - Flashes 300ms on a wrong move
- **LED_ACTIVE** - On for the duration of an active cook

### Audio Feedback (via `tone()`)
- **Correct Move**: 800Hz beep (100ms)
- **Wrong Move**: 400Hz beep (300ms)
- **Cooking Done**: 1200Hz beep (200ms)

## Technical Details

### Input Detection
- **Polling rate**: input is sampled at 100Hz (`INPUT_INTERVAL_MS`), decoupled from the display, which redraws at 25Hz (`RENDER_INTERVAL_MS`). Both used to share one 20Hz loop tied to how often a frame got pushed to the OLED - fast gestures (especially FLICK, and edges near the end of a SHAKE/ROTATE) could land between samples and just never register. Polling input independently of rendering fixed that.
- **Joystick Deadzone**: +/-75 (out of the library's 0-1023 range)
- **Hold Steady**: 500ms continuously centered, with no click and no pending gesture
- **Rapid Shake**: 3 direction edges that strictly alternate between two opposite directions (e.g. L,R,L, one full back-and-forth), and all 3 must land within 1 second (`SHAKE_WINDOW_MS`) - otherwise slow unrelated pushes that happen to alternate could pile up into a "shake"
- **Rotate**: 4 direction edges - one full lap around the four directions - within the 1000ms gesture window. Made more forgiving in two ways: it's one lap instead of the old 5 edges, and once the stick has changed direction twice the buffer waits `MULTI_EDGE_CONFIRM_MS` (450ms) instead of `PLAIN_CONFIRM_MS` (200ms) before giving up and calling it a plain push, so a slower rotate has time to finish. A single push still resolves at the snappy 200ms.
- **Press vs. Flick**: on a click-and-hold, whichever happens first wins - an upward stick push registers FLICK immediately, otherwise a 300ms hold registers PRESS. Flick is also forgiving about timing: an upward push that lands on the very same tick as the click, or up to 250ms after the click releases (in case the flick's peak arrives just after the button springs back), still counts
- **Plain directional moves**: confirmed 200ms after the last direction edge, so a real shake/rotate has time to be recognized instead
- **Post-move settle grace**: after any move registers, input is ignored until the stick has *continuously* rested at center (and the button is up) for `SETTLE_MS`. This matters because a physical shake or rotate keeps swinging after the gesture has already been recognized - without the grace window, that leftover motion would be read as a (wrong) attempt at the move that just came up. Momentarily blipping through center while still swinging doesn't satisfy it; the stick has to actually come to rest.
- **Duplicate-edge filtering**: a direction edge that repeats the direction just recorded (e.g. the stick dips back through a corner of the deadzone mid-push, or the ADC reading jitters) is not logged as a new edge. Without this, that kind of noise could inflate a plain push into a false ROTATE.
- **Serial move trace**: every registered move (right or wrong) prints a line like `read SHAKE want ROTATE miss` to the serial monitor - the fastest way to tell whether a "move that didn't register" was never detected at all, or was detected as the wrong move.

### Game States
- `STATE_MENU` - waiting for the joystick click to start
- `STATE_COOKING` - cook timer counting down, input detection active, wrong-move counter tracked in the background
- `STATE_READY` - cooked; 10s grace period to click and take it off before it burns
- `STATE_DONE` - successfully taken off in time, shows final score; a click starts cooking again
- `STATE_FIRE` - burnt (10s grace period expired, or 10 total wrong moves reached); a click starts cooking again

## Customization Options
All in `src/FryingPanSimonSays.cpp`, near the top:
```cpp
#define COOK_TIME_MS         30000UL // cook timer counts down from this in real time
#define CHAIN_BONUS_STEP_MS  500UL   // each move in a correct-move chain removes this much more...
#define CHAIN_BONUS_CAP_MS   3000UL  // ...capped at this much per move
#define MISS_INDICATOR_MS    250UL   // how long the small "wrong move" mark stays on screen
#define READY_BURN_TIMEOUT_MS 10000UL // grace period to pick it up before it burns, once cooked
#define WRONG_MOVES_TO_BURN  10       // cumulative wrong moves (not necessarily in a row) before it catches fire

#define JOYSTICK_DEADZONE    75      // out of 0-1023
#define HOLD_STEADY_MS       500UL
#define PRESS_HOLD_MS        250UL
#define SHAKE_ALTERNATIONS   3       // edges needed for a shake; lower = more forgiving
#define SHAKE_WINDOW_MS      1000UL  // those edges must all land within this
#define ROTATE_EVENTS        4       // 4 edges = one full lap around the four directions
#define GESTURE_WINDOW_MS    1000UL
#define PLAIN_CONFIRM_MS     130UL
#define MULTI_EDGE_CONFIRM_MS 320UL  // longer settle once a multi-direction gesture is underway
#define FLICK_RELEASE_GRACE_MS 250UL // flick landing just after click release still counts
#define SETTLE_MS            90UL    // stick must rest at center this long before the next move is judged
#define MAX_GESTURE_EVENTS   10      // capacity of the gesture buffer

#define INPUT_INTERVAL_MS    10UL    // 100Hz input polling
#define RENDER_INTERVAL_MS   40UL    // 25Hz display refresh
```

## Installation Steps
1. **Install PlatformIO** (VS Code extension, or `pip install platformio`)
2. Open this folder (`FryingPan_SimonSays/`) as a PlatformIO project
3. Connect the ESP32 and run `pio run -t upload` (or use the PlatformIO IDE Upload button)
4. `pio device monitor` to view serial logs

## Testing Checklist
- [ ] Move screen lights up at startup and shows the title - this alone must work, or check GPIO32/33 wiring
- [ ] If the bar screen is connected, it also lights up and blinks "PRESS TO COOK"; if not, serial prints "Bar SSD1306 not found" once at boot and the game still runs normally on the move screen's own bar
- [ ] Joystick click starts cooking (30.0s shown on the move screen's compact bar, and at double size on the bar screen if connected)
- [ ] Single directional moves (^v<>) recognized, ball animation holds out toward the pushed edge for ~500ms rather than oscillating
- [ ] Hold steady recognized (500ms centered), ball pulses slowly in place
- [ ] Rapid shake recognized after one back-and-forth (3 alternating edges), ball slides left-right briskly (the one intentionally fast animation)
- [ ] Alternating pushes spread out over more than 1 second are NOT read as a shake
- [ ] Rotation recognized after a single full lap (4 direction changes), ball orbits the socket
- [ ] A slower rotate still completes instead of being cut short and read as a plain push
- [ ] Click hold (press) recognized, ball dips down and holds
- [ ] Click + flick up recognized even if the flick lands just before/after the click; animation shows the press, then a snap up with ghost-ball trail
- [ ] Correct moves show the small circle mark top-left, light green LED, play 800Hz beep, subtract the next chain bonus from the timer, and roll a new move
- [ ] Chain bonus climbs 0.5s -> 1.0s -> 1.5s ... capping at 3.0s with repeated correct moves
- [ ] Wrong moves show the small square mark top-right, light red LED, play 400Hz beep, reset the chain to 0.5s, and re-prompt the same move (timer unaffected)
- [ ] No score or next-bonus text shown during cooking - move screen shows only the animation, bar screen only the bar
- [ ] Finishing a shake or rotate doesn't immediately count as a wrong move on the next prompt while the stick is still swinging (post-move settle grace)
- [ ] Quick flicks and the tail end of fast shakes/rotates register reliably now (100Hz input polling) - if moves still feel dropped, watch the serial monitor: a printed `read X want Y` line means it WAS detected (just as the wrong move), no line at all means the gesture never triggered
- [ ] Both bars (move screen's compact one and the bar screen's big one, if connected) depletes in real time, visibly jumps forward on each correct move, and their overlaid time text stays readable over both the filled and empty portions
- [ ] Timer hitting 0 shows "COOKED! Take it off!" with a 10s grace bar; clicking in time reaches the done screen (moves-hit score, click starts a new cook)
- [ ] Letting the 10s grace period expire without clicking sets it on fire (flame animation + blinking "FIRE FIRE FIRE FIRE")
- [ ] Racking up 10 wrong moves total during a single cook (spread out, not necessarily consecutive) also sets it on fire immediately
- [ ] Clicking on the fire screen starts a fresh cook

## Troubleshooting

**Move screen not displaying:**
- Fatal - `fryingPanSetup()` halts in a loop and prints "Move SSD1306 init failed" if this one doesn't come up, since the game can't run without it
- Check I2C address (`OLED_ADDRESS`, typically 0x3C) and wiring on GPIO32/33, plus pull-up resistors (most modules have them onboard)

**Bar screen not displaying:**
- Not fatal - serial prints "Bar SSD1306 not found - running on the move screen only" and the game continues normally
- Check wiring on GPIO25/26 if you expect it to be connected

**Joystick directions feel backwards / swapped:**
- `dirToDirection()` in `main.cpp` assumes a higher Y reading means "pushed up" - flip the comparison there if your module is wired the other way

**Moves not detected reliably:**
- Watch the serial monitor while playing: a `read X want Y` line means the move WAS detected (just not the right one) - the fix is in the gesture-recognition tuning below. No line at all means it never triggered - the fix is `INPUT_INTERVAL_MS` (should already be fast enough at 100Hz) or a wiring/deadzone issue on the joystick itself
- Adjust `JOYSTICK_DEADZONE` (remember the library reports 0-1023, not the ESP32's native 0-4095)
- Adjust `SHAKE_ALTERNATIONS` / `SHAKE_WINDOW_MS` / `ROTATE_EVENTS` / `GESTURE_WINDOW_MS` / `MULTI_EDGE_CONFIRM_MS` for the physical joystick's feel

**Sound not playing:**
- Verify the speaker is on GPIO 25 and check its polarity
