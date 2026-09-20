#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <MFRC522.h>
#include <OvercookedComm.h>
#include <PresenceReader.h>
#include <SPI.h>
#include <Station.h>
#include <WiFi.h>

#include "PressTask.h"

// Cutting board station. The game rules (how many presses a tomato needs,
// what can be chopped) live on the server; this file is only the wiring.

// Standard RC522 breakout (SDA/SCK/MOSI/MISO/IRQ/GND/RST/3.3V) only
// supports SPI, not I2C. On the ESP32-WROVER-IE dev board every RC522
// signal is wired to the LEFT-hand header, using the GPIO matrix to
// remap SPI onto pins 32/33/25/26 (top to bottom on the header):
//   RC522 SDA  -> GPIO32  (SS/CS)
//   RC522 SCK  -> GPIO33
//   RC522 MOSI -> GPIO25
//   RC522 MISO -> GPIO26
//   RC522 RST  -> GPIO27
//   RC522 GND  -> GND     (left header, between GPIO12 and GPIO13)
//   RC522 3.3V -> 3V3     (top of left header; NOT 5V, the chip is not 5V tolerant)
//   RC522 IRQ  -> not connected
//
// Deliberately skipped on the left header: GPIO34/35/36(VP)/39(VN) are
// input-only, GPIO12 is a boot strapping pin, and GPIO9/10/11 (D2/D3/CMD)
// are wired to the module's flash.
#define SS_PIN   32
#define SCK_PIN  33
#define MOSI_PIN 25
#define MISO_PIN 26
#define RST_PIN  27

// WS2812B strip (5 pixels), also on the left header:
//   Strip DIN -> GPIO13   (via ~330 ohm series resistor, next to the GND pin)
//   Strip 5V  -> 5V       (bottom pin of left header, USB 5V)
//   Strip GND -> GND      (must share ground with the ESP32)
// Despite the "SPI" in the product name, WS2812B is a single-wire protocol:
// only DIN is needed (DOUT is for chaining more pixels).
#define LED_PIN        13
#define LED_COUNT      5
#define LED_BRIGHTNESS 80 // 0-255

// Limit switch, see PressTask.h.
#define SWITCH_PIN  14
#define DEBOUNCE_MS 30

MFRC522 rfid(SS_PIN, RST_PIN);
tagreader::PresenceReader reader(rfid);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
station::Display display(strip, LED_BRIGHTNESS);
PressTask pressTask(SWITCH_PIN, DEBOUNCE_MS);
station::Station cuttingBoard(oc::StationKind::CuttingBoard, reader, display, &pressTask);

void setup() {
  Serial.begin(115200);

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, -1); // CS is driven by MFRC522 via SS_PIN
  if (!reader.begin()) Serial.println("RC522 not answering, check the wiring");

  if (!oc::begin()) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }
  cuttingBoard.begin();

  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());
}

void loop() {
  cuttingBoard.update(millis());
}
