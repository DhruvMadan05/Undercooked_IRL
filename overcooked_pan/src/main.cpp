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

// SSD1306 OLED (128x64, I2C 4-pin module): shows the gesture to do, the time
// bar and BURNT! (see PatternOled.h). The ESP32's default I2C pins, free on
// this station (RC522 is on 32/33/25/26/27, the LED strip on 13, the joystick
// on 34/35), and the same pair the deep fryer's screen uses.
//   OLED GND -> GND
//   OLED VCC -> 3V3
//   OLED SDA -> GPIO21
//   OLED SCL -> GPIO22
// Address 0x3C, or 0x3D on some clones; begin() probes both and says on serial
// what it found.
#define OLED_SDA_PIN 21
#define OLED_SCL_PIN 22

JoystickPatternTask joystickTask(JOY_X_PIN, JOY_Y_PIN, JOY_DEAD_ZONE, INVERT_X, INVERT_Y);
pan::PatternOled oled(OLED_SDA_PIN, OLED_SCL_PIN);
station::StandardStation fryingPan(oc::StationKind::Pan, &joystickTask, &oled);

void setup() {
  Serial.begin(115200);
  fryingPan.begin();
}

void loop() {
  fryingPan.update(millis());
}
