#pragma once
// A station with the standard hardware (StandardWiring.h) in one object, so a
// station's main.cpp is only its kind and its task:
//
//   PressTask task(14, 30);
//   station::StandardStation board(oc::StationKind::CuttingBoard, &task);
//   void setup() { Serial.begin(115200); board.begin(); }
//   void loop()  { board.update(millis()); }
//
// Pass no task for a station with nothing to measure (plate, delivery).

#include <Adafruit_NeoPixel.h>
#include <MFRC522.h>
#include <PresenceReader.h>
#include <SPI.h>
#include <WiFi.h>

#include "Display.h"
#include "StandardWiring.h"
#include "Station.h"
#include "StationTask.h"

namespace station {

class StandardStation {
 public:
  StandardStation(oc::StationKind kind, StationTask *task = nullptr)
      : rfid_(wiring::kRfidSs, wiring::kRfidRst),
        reader_(rfid_),
        strip_(wiring::kLedCount, wiring::kLedPin, NEO_GRB + NEO_KHZ800),
        display_(strip_, wiring::kLedBrightness),
        station_(kind, reader_, display_, task) {}

  // Call from setup(), after Serial.begin().
  void begin() {
    // CS is driven by MFRC522 via kRfidSs, so SPI gets no hardware CS pin.
    SPI.begin(wiring::kRfidSck, wiring::kRfidMiso, wiring::kRfidMosi, -1);
    if (!reader_.begin()) Serial.println("RC522 not answering, check the wiring");

    if (!oc::begin()) {
      Serial.println("ESP-NOW init failed");
      while (true) delay(1000);
    }
    station_.begin();

    Serial.print("My MAC: ");
    Serial.println(WiFi.macAddress());
  }

  void update(uint32_t now) { station_.update(now); }

 private:
  MFRC522 rfid_;
  tagreader::PresenceReader reader_;
  Adafruit_NeoPixel strip_;
  Display display_;
  Station station_;
};

} // namespace station
