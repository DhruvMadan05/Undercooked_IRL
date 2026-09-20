#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// Deep fryer station: mimics Stardew Valley's fishing minigame. Scan an
// item's tag to start a fry, then keep your hand's height over the
// HC-SR04 aligned with a roaming target zone (the "fish") to fill the fry
// progress meter before it drains to zero. On success the item's UID is
// broadcast over ESP-NOW, same as the other stations.

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

// WS2812B strip. Bigger than the other stations' 5 pixels: this strip has
// to show two independently-moving markers (the target zone and the
// player's hand position), which needs more resolution to read clearly.
//   Strip DIN -> GPIO13   (via ~330 ohm series resistor)
//   Strip 5V  -> 5V, Strip GND -> GND
#define LED_PIN   13
#define LED_COUNT 16

// HC-SR04 ultrasonic, reads the height of the player's hand/basket handle
// above the fryer.
//   HC-SR04 VCC  -> 5V              (the sensor itself needs 5V, not 3.3V)
//   HC-SR04 GND  -> GND
//   HC-SR04 TRIG -> GPIO4           (direct -- ESP32's 3.3V output is a valid trigger level)
//   HC-SR04 ECHO -> GPIO35 through a voltage divider: ECHO -> 1k -> node,
//                   node -> 2k -> GND, node -> GPIO35. ECHO pulses at 5V,
//                   which will damage the ESP32's 3.3V-only GPIO inputs
//                   without the divider.
// GPIO35 is an input-only ADC1 pin (same family as the sink station's
// joystick pins) -- fine here since we only ever read it, never drive it.
#define TRIG_PIN 4
#define ECHO_PIN 35

// How long to wait for an echo before giving up on a ping (us). ~30ms
// covers ~5m round trip, far more range than this sensor needs at fryer
// distance.
#define PING_TIMEOUT_US 30000UL

// Minimum time between pings. The HC-SR04 needs quiet time for its own
// echo to die out, or the next trigger hears the previous one.
#define PING_INTERVAL_MS 60

// Hand-height range we care about, in cm. Closer than NEAR_CM clamps to
// "top" (1.0); farther than FAR_CM clamps to "bottom" (0.0). Tune to the
// physical mounting height.
#define NEAR_CM 3.0f
#define FAR_CM  30.0f

// How much of the roaming range the catch zone covers, as a fraction of
// the full range. Wider = easier, same idea as a bigger bar for an easier
// fish in the original game.
#define CATCH_ZONE_FRAC 0.20f

// Target ("fish") motion tuning -- mostly drifts, occasionally darts.
#define TARGET_DRIFT_ACCEL 0.6f      // fraction/sec^2, pull toward a wandering velocity
#define TARGET_MAX_SPEED   0.5f      // fraction/sec, normal drift speed
#define TARGET_DART_CHANCE_PER_TICK 200 // out of 10000, checked once per FRYING tick
#define TARGET_DART_SPEED   1.6f     // fraction/sec, during a dart
#define TARGET_DART_MS       250     // how long a dart lasts

// Fry progress tuning.
#define PROGRESS_FILL_PER_SEC  0.35f  // while hand overlaps the target
#define PROGRESS_DRAIN_PER_SEC 0.25f  // while it doesn't

#define SAMPLE_INTERVAL_MS 60 // matches the ultrasonic's own ping interval

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

enum StationState { IDLE, FRYING, DONE_SUCCESS, DONE_FAIL };
StationState state = IDLE;

uint8_t capturedUid[10];
uint8_t capturedUidSize = 0;

float handPos = 0.5f;    // smoothed 0..1, 0 = FAR_CM, 1 = NEAR_CM
float targetPos = 0.5f;  // 0..1, the roaming "fish"
float targetVel = 0.0f;  // fraction of range per second
bool darting = false;
uint32_t dartEndsAtMs = 0;

float fryProgress = 0.0f; // 0..1
uint32_t lastSampleMs = 0;
uint32_t lastPingMs = 0;

// Fires one ping and returns distance in cm, or -1 if no echo came back
// in time (out of range / nothing there).
float pingDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long durationUs = pulseIn(ECHO_PIN, HIGH, PING_TIMEOUT_US);
  if (durationUs == 0) return -1;
  return durationUs / 58.0f; // standard HC-SR04 us-to-cm conversion
}

// Converts a raw distance into a smoothed 0..1 hand position, closer =
// higher. A missed ping (-1) just skips the update instead of snapping
// to an endpoint.
void updateHandPos() {
  float distanceCm = pingDistanceCm();
  if (distanceCm < 0) return;

  float raw = (FAR_CM - distanceCm) / (FAR_CM - NEAR_CM);
  raw = constrain(raw, 0.0f, 1.0f);

  const float smoothing = 0.35f; // exponential smoothing, lower = smoother/slower
  handPos += (raw - handPos) * smoothing;
}

// Advances the roaming target by dt seconds: mostly a smooth random walk,
// occasionally a fast dart in a random direction, mimicking a fish that
// drifts but sometimes bolts.
void updateTarget(float dt) {
  uint32_t now = millis();

  if (darting && now >= dartEndsAtMs) {
    darting = false;
  }

  if (!darting && random(0, 10000) < TARGET_DART_CHANCE_PER_TICK) {
    darting = true;
    dartEndsAtMs = now + TARGET_DART_MS;
    targetVel = (random(0, 2) == 0 ? -1.0f : 1.0f) * TARGET_DART_SPEED;
  } else if (!darting) {
    float wanderTo = (random(-1000, 1001) / 1000.0f) * TARGET_MAX_SPEED;
    float accel = TARGET_DRIFT_ACCEL * dt;
    if (targetVel < wanderTo) targetVel = min(targetVel + accel, wanderTo);
    else targetVel = max(targetVel - accel, wanderTo);
  }

  targetPos += targetVel * dt;

  // Bounce off the ends instead of clamping, so it doesn't just sit
  // pinned at 0 or 1.
  if (targetPos < 0.0f) { targetPos = -targetPos; targetVel = -targetVel; }
  if (targetPos > 1.0f) { targetPos = 2.0f - targetPos; targetVel = -targetVel; }
}

// LED pixel granularity is coarser than the game logic below -- overlap
// for scoring is computed on the raw floats, not on which pixel lights up.
void renderFryingLeds() {
  strip.clear();

  int targetPixel = (int)roundf(targetPos * (LED_COUNT - 1));
  int handLo = (int)roundf((handPos - CATCH_ZONE_FRAC / 2) * (LED_COUNT - 1));
  int handHi = (int)roundf((handPos + CATCH_ZONE_FRAC / 2) * (LED_COUNT - 1));
  handLo = constrain(handLo, 0, LED_COUNT - 1);
  handHi = constrain(handHi, 0, LED_COUNT - 1);

  for (int i = handLo; i <= handHi; i++) {
    strip.setPixelColor(i, strip.Color(120, 60, 0)); // basket zone: amber
  }

  bool overlap = (targetPixel >= handLo && targetPixel <= handHi);
  strip.setPixelColor(targetPixel, overlap ? strip.Color(255, 255, 255)
                                            : strip.Color(0, 180, 220)); // fish: cyan, white when caught

  // Dim green wash under whatever isn't already lit, filled left-to-right
  // by fry progress -- a simple readout without needing a second strip.
  int progressPixels = (int)roundf(fryProgress * LED_COUNT);
  for (int i = 0; i < progressPixels; i++) {
    if (strip.getPixelColor(i) == 0) strip.setPixelColor(i, strip.Color(0, 40, 0));
  }

  strip.show();
}

void flashResult(bool success) {
  uint32_t color = success ? strip.Color(0, 150, 0) : strip.Color(150, 0, 0);
  for (int i = 0; i < 3; i++) {
    strip.fill(color);
    strip.show();
    delay(150);
    strip.clear();
    strip.show();
    delay(150);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

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

  randomSeed(esp_random());

  Serial.println("Deep fryer ready. Scan an item to start frying...");
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
      Serial.println(" - start frying!");

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      handPos = 0.5f;
      targetPos = 0.5f;
      targetVel = 0.0f;
      darting = false;
      fryProgress = 0.0f;
      lastSampleMs = millis();
      lastPingMs = 0;
      state = FRYING;
      break;
    }

    case FRYING: {
      uint32_t now = millis();
      if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
      float dt = (now - lastSampleMs) / 1000.0f;
      lastSampleMs = now;

      if (now - lastPingMs >= PING_INTERVAL_MS) {
        lastPingMs = now;
        updateHandPos();
      }

      updateTarget(dt);

      float catchLo = handPos - CATCH_ZONE_FRAC / 2;
      float catchHi = handPos + CATCH_ZONE_FRAC / 2;
      bool overlap = (targetPos >= catchLo && targetPos <= catchHi);

      fryProgress += (overlap ? PROGRESS_FILL_PER_SEC : -PROGRESS_DRAIN_PER_SEC) * dt;
      fryProgress = constrain(fryProgress, 0.0f, 1.0f);

      renderFryingLeds();

      if (fryProgress >= 1.0f) state = DONE_SUCCESS;
      else if (fryProgress <= 0.0f) state = DONE_FAIL;
      break;
    }

    case DONE_SUCCESS: {
      Serial.print("Perfectly fried! UID:");
      printUidHex(capturedUid, capturedUidSize);
      Serial.println();

      flashResult(true);

      outgoing.size = capturedUidSize;
      memcpy(outgoing.uid, capturedUid, capturedUidSize);
      esp_now_send(broadcastAddress, (uint8_t *)&outgoing, sizeof(outgoing));

      strip.clear();
      strip.show();
      state = IDLE;
      break;
    }

    case DONE_FAIL: {
      Serial.print("Burnt! UID:");
      printUidHex(capturedUid, capturedUidSize);
      Serial.println();

      flashResult(false);

      strip.clear();
      strip.show();
      state = IDLE;
      break;
    }
  }
}
