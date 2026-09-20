#include <Arduino.h>
#include "FryingPanSimonSays.h"
#include "JoystickTest.h"
#include "RfidTagReader.h"

// Joystick wiring confirmed via JoystickTest - now running the actual game.
// Swap back to joystickTestSetup()/joystickTestLoop() to re-test wiring, or
// rfidSetup()/rfidLoop() for the RFID tag reader.
//
// TODO: FryingPanSimonSays.cpp still needs real calibration and likely more
// edits once the joystick/pan/screens are mechanically assembled into the
// actual prop - current tunables are best-guess from a bare dev board.

void setup() {
  fryingPanSetup();
}

void loop() {
  fryingPanLoop();
}
