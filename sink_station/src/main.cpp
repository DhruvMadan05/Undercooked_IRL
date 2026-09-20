#include <Arduino.h>
#include <StandardStation.h>

#include "ScrubTask.h"

// Sink station: put a dirty plate's tag on the reader, then scrub the joystick in
// circles until the plate is clean. How long that takes is set on the server
// (level.toml, wash_s); this file is only the wiring.
// RC522 reader and LED strip: see shared/StationCore/src/StandardWiring.h.
//
// Joystick module (analog, on the left header; these pins are input-only, which is fine).
// They are ADC1 pins on purpose: ADC2 (the pins on the right header) stops working
// as soon as WiFi is on, and ESP-NOW needs WiFi.
//   VRx (X axis) -> GPIO34
//   VRy (Y axis) -> GPIO35
//   +5V / VCC    -> 3V3     (NOT 5V, see ScrubTask.h)
//   GND          -> GND
//   SW (button)  -> not used (any free digital pin would do if you want it later)
#define JOY_X_PIN 34
#define JOY_Y_PIN 35

ScrubTask scrubTask(JOY_X_PIN, JOY_Y_PIN);
station::StandardStation sink(oc::StationKind::Sink, &scrubTask);

void setup() {
  Serial.begin(115200);
  sink.begin();
}

void loop() {
  sink.update(millis());
}
