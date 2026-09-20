#include <Arduino.h>
#include <StandardStation.h>

#include "JoystickPatternTask.h"
#include "PatternOled.h"

// Frying pan station. Which foods can be fried, which pattern they need and how
// many steps it takes are set on the server (level.toml); this file is only the wiring.
// RC522 reader and LED strip: see shared/StationCore/src/StandardWiring.h.
//
// Joystick module (analog, on the left header; these pins are input-only, which is fine):
//   VRx (X axis) -> GPIO34
//   VRy (Y axis) -> GPIO35
//   +5V / VCC    -> 3V3     (NOT 5V, see JoystickPatternTask.h)
//   GND          -> GND
//   SW (button)  -> not used
// The pins are a first guess: change them here to match how you wire it. If a
// pattern counts when you push the stick the wrong way, flip INVERT_X / INVERT_Y.
#define JOY_X_PIN   34
#define JOY_Y_PIN   35
#define JOY_DEAD_ZONE 0.6f // how far to push (0-1) before it counts as a direction
#define INVERT_X    false
#define INVERT_Y    false

JoystickPatternTask joystickTask(JOY_X_PIN, JOY_Y_PIN, JOY_DEAD_ZONE, INVERT_X, INVERT_Y);
station::StandardStation fryingPan(oc::StationKind::Pan, &joystickTask);

void setup() {
  Serial.begin(115200);
  fryingPan.begin();
}

void loop() {
  fryingPan.update(millis());
}
