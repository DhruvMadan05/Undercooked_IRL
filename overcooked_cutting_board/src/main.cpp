#include <Arduino.h>
#include <StandardStation.h>

#include "PressTask.h"

// Cutting board station. The game rules (how many presses a tomato needs,
// what can be chopped) live on the server; this file is only the wiring.
// RC522 reader and LED strip: see shared/StationCore/src/StandardWiring.h.
//
// Limit switch (left header, between GPIO27 and GPIO12), see PressTask.h:
//   Switch COM -> GPIO14
//   Switch NO  -> GND     (left header GND, next to GPIO12/GPIO13)
#define SWITCH_PIN  14
#define DEBOUNCE_MS 30

PressTask pressTask(SWITCH_PIN, DEBOUNCE_MS);
station::StandardStation cuttingBoard(oc::StationKind::CuttingBoard, &pressTask);

void setup() {
  Serial.begin(115200);
  cuttingBoard.begin();
}

void loop() {
  cuttingBoard.update(millis());
}
