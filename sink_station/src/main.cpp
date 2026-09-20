#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// Sink station: scan an item's tag, then scrub a KY-023 joystick in circles
// for WASH_SECONDS of *active* motion to wash it. On completion the item's
// UID is broadcast over ESP-NOW, same as the other stations.

// RC522, same left-header wiring as the other stations:
//   RC522 SDA  -> GPIO32  (SS/CS)
//   RC522 SCK  -> GPIO33
//   RC522 MOSI -> GPIO25
//   RC522 MISO -> GPIO26
//   RC522 RST  -> GPIO27
//   RC522 GND  -> GND     (left header, between GPIO12 and GPIO13)
//   RC522 3.3V -> 3V3     (top of left header; NOT 5V, the chip is not 5V tolerant)
#define SS_PIN   32
#define SCK_PIN  33
#define MOSI_PIN 25
#define MISO_PIN 26
#define RST_PIN  27

// WS2812B strip (5 pixels), same as the other stations:
//   Strip DIN -> GPIO13   (via ~330 ohm series resistor)
//   Strip 5V  -> 5V, Strip GND -> GND
#define LED_PIN   13
#define LED_COUNT 5

// KY-023 joystick.
// You asked to wire this on the right-hand header (the one running
// CLK...GND) but that header's usable GPIOs are all ADC2 (0/2/4/15/etc).
// ADC2 shares hardware with the WiFi radio and becomes unreliable as soon
// as WiFi is running -- which it always is here, since ESP-NOW needs it.
// So VRx/VRy are wired to GPIO34/35 instead: they're ADC1 (unaffected by
// WiFi), input-only (fine for a joystick pot, which is read-only anyway),
// and they're free pins sitting just above the RC522 pins on the LEFT
// header. SW (the joystick's click button) isn't used by this sketch, but
// if you want it later, any free right-header GPIO (e.g. 18) works fine
// since it's a plain digital input.
#define VRX_PIN 34
#define VRY_PIN 35

// How long the joystick must be *actively* moving in a circle to finish
// washing an item.
#define WASH_SECONDS 5.0f
#define WASH_DURATION_MS ((uint32_t)(WASH_SECONDS * 1000))

// How far off-center (in raw ADC counts, ~0-4095) the stick has to be
// before its angle counts for anything. Filters out center-noise.
#define DEADZONE_RADIUS 500.0f

// Minimum angle change per sample to count as "actively scrubbing" rather
// than just holding the stick off to one side.
#define MIN_DELTA_ANGLE 0.05f // radians (~3 degrees)
#define SAMPLE_INTERVAL_MS 20

MFRC522 rfid(SS_PIN, RST_PIN);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
  uint8_t size;
  uint8_t uid[10]; // MFRC522 UIDs are at most 10 bytes
} TagMessage;

TagMessage outgoing;

void printUidHex(const uint8_t *uid, uint8_t size) {
  for (uint8_t i = 0; i < size; i++) {
    Serial.printf(" %02X", uid[i]);
  }
}

enum StationState { IDLE, WASHING, DONE };
StationState state = IDLE;

uint8_t capturedUid[10];
uint8_t capturedUidSize = 0;

float centerX = 2048, centerY = 2048;
bool haveLastAngle = false;
float lastAngle = 0;
uint32_t washActiveMs = 0;
uint32_t lastSampleMs = 0;

// Average a handful of samples with the stick assumed at rest, so we don't
// depend on the pot being perfectly centered at 2048/2048.
void calibrateJoystick() {
  long sumX = 0, sumY = 0;
  const int samples = 20;
  for (int i = 0; i < samples; i++) {
    sumX += analogRead(VRX_PIN);
    sumY += analogRead(VRY_PIN);
    delay(2);
  }
  centerX = sumX / (float)samples;
  centerY = sumY / (float)samples;
  Serial.printf("Joystick center: %.0f, %.0f\n", centerX, centerY);
}

// Returns the stick's angle around center, and whether it's far enough
// from center for that angle to be meaningful.
float readStickAngle(bool *valid) {
  float dx = analogRead(VRX_PIN) - centerX;
  float dy = analogRead(VRY_PIN) - centerY;
  float radius = sqrtf(dx * dx + dy * dy);
  if (radius < DEADZONE_RADIUS) {
    *valid = false;
    return 0;
  }
  *valid = true;
  return atan2f(dy, dx);
}

void updateProgressLeds() {
  float frac = (float)washActiveMs / WASH_DURATION_MS;
  if (frac > 1) frac = 1;
  uint8_t lit = (uint8_t)roundf(frac * LED_COUNT);
  strip.clear();
  for (uint8_t i = 0; i < lit; i++) {
    strip.setPixelColor(i, strip.Color(0, 80, 120)); // wash-in-progress blue
  }
  strip.show();
}

void flashSuccess() {
  for (int i = 0; i < 3; i++) {
    strip.fill(strip.Color(0, 150, 0));
    strip.show();
    delay(150);
    strip.clear();
    strip.show();
    delay(150);
  }
}

void setup() {
  Serial.begin(115200);

  strip.begin();
  strip.clear();
  strip.show();

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, -1); // CS is driven by MFRC522 via SS_PIN
  rfid.PCD_Init();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
  }

  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());

  calibrateJoystick();
  Serial.println("Sink ready. Scan an item to wash it...");
}

void loop() {
  switch (state) {
    case IDLE: {
      if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
        return;
      }

      capturedUidSize = rfid.uid.size;
      memcpy(capturedUid, rfid.uid.uidByte, capturedUidSize);
      Serial.print("Item placed, UID:");
      printUidHex(capturedUid, capturedUidSize);
      Serial.println(" - start scrubbing in circles!");

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      calibrateJoystick(); // assumes the stick is at rest right now
      washActiveMs = 0;
      haveLastAngle = false;
      lastSampleMs = millis();
      state = WASHING;
      break;
    }

    case WASHING: {
      uint32_t now = millis();
      if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
      uint32_t dt = now - lastSampleMs;
      lastSampleMs = now;

      bool valid;
      float angle = readStickAngle(&valid);

      // TEMP DEBUG — remove once working
      static uint32_t lastDebugMs = 0;
      if (now - lastDebugMs >= 200) {
        lastDebugMs = now;
        Serial.printf("raw=(%d,%d) valid=%d angle=%.2f activeMs=%lu\n",
                      analogRead(VRX_PIN), analogRead(VRY_PIN),
                      valid, angle, washActiveMs);
}
      if (valid && haveLastAngle) {
        float delta = angle - lastAngle;
        if (delta > PI) delta -= 2 * PI;
        if (delta < -PI) delta += 2 * PI;
        if (fabsf(delta) >= MIN_DELTA_ANGLE) {
          washActiveMs += dt;
        }
      }
      // If the stick passed back through the deadzone, drop the last angle
      // so the next reading doesn't register as one huge jump.
      haveLastAngle = valid;
      if (valid) lastAngle = angle;

      updateProgressLeds();

      if (washActiveMs >= WASH_DURATION_MS) {
        state = DONE;
      }
      break;
    }

    case DONE: {
      Serial.print("Wash complete, UID:");
      printUidHex(capturedUid, capturedUidSize);
      Serial.println();

      flashSuccess();

      outgoing.size = capturedUidSize;
      memcpy(outgoing.uid, capturedUid, capturedUidSize);
      esp_now_send(broadcastAddress, (uint8_t *)&outgoing, sizeof(outgoing));

      strip.clear();
      strip.show();
      state = IDLE;
      break;
    }
  }
}
