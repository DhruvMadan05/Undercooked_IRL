#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// Deep fryer station: mimics Stardew Valley's fishing minigame. Scan an
// item's tag to start a fry, then keep your hand's height over the
// HC-SR04 aligned with a roaming target zone (the "fish") to fill the fry
// progress meter before it drains to zero. On success the item's UID is
// broadcast over ESP-NOW, same as the other stations.
//
// The OLED draws the target zone as a bar and the hand as a box on the same
// near/far axis (left = near the sensor, right = far); the LED strip is
// dedicated entirely to the fry progress bar.

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

// WS2812B strip, used purely as the fry progress bar now that the OLED
// carries the target-tracking readout.
//   Strip DIN -> GPIO13   (via ~330 ohm series resistor)
//   Strip 5V  -> 5V, Strip GND -> GND
#define LED_PIN   13
#define LED_COUNT 16

// 0.91" SSD1306 OLED (128x32), I2C, "Ver 1.6" 4-pin module. Shows the
// target zone's position and which direction the hand needs to move to
// reach it.
//   OLED GND -> GND
//   OLED VCC -> 3V3     (these modules are 3.3V logic; check the silkscreen
//                        before trying 5V even if it has an onboard regulator)
//   OLED SCL -> GPIO22  (I2C clock, ESP32's default)
//   OLED SDA -> GPIO21  (I2C data, ESP32's default)
// Default I2C address for these boards is 0x3C. If the screen stays blank,
// run an I2C scanner sketch first -- a few clones ship as 0x3D instead.
#define OLED_SDA_PIN  21
#define OLED_SCL_PIN  22
#define OLED_WIDTH    128
#define OLED_HEIGHT   32
#define OLED_RESET    -1     // no dedicated reset pin on this module
#define OLED_ADDRESS  0x3C
#define OLED_UPDATE_INTERVAL_MS 150 // I2C full-frame pushes are slower than the LED strip; throttle separately

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
// fish in the original game. Widened along with everything else below to
// make the round very forgiving.
#define CATCH_ZONE_FRAC 0.30f

// Target ("fish") motion tuning -- mostly drifts, occasionally darts.
// Slowed down (and darts made rarer/gentler) so the target doesn't demand
// quick reactions to stay on top of.
#define TARGET_DRIFT_ACCEL 0.4f      // fraction/sec^2, pull toward a wandering velocity
#define TARGET_MAX_SPEED   0.3f      // fraction/sec, normal drift speed
#define TARGET_DART_CHANCE_PER_TICK 100 // out of 10000, checked once per FRYING tick
#define TARGET_DART_SPEED   1.0f     // fraction/sec, during a dart
#define TARGET_DART_MS       200     // how long a dart lasts

// Fry progress tuning: 10 seconds of continuous overlap to go from empty
// to fully fried, 60 seconds of continuous miss to go from full to burnt.
#define PROGRESS_FILL_PER_SEC  (1.0f / 10.0f)
#define PROGRESS_DRAIN_PER_SEC (1.0f / 60.0f)
#define FRY_START_PROGRESS 0.5f  // start in the middle, like the bar it's modeled on -- starting at
                                  // 0.0 gives zero buffer, so any miss on the very first tick is an
                                  // instant fail before the player's hand is even in position
#define FRY_GRACE_MS 750         // scoring is suspended for this long after a scan, so the ultrasonic
                                  // gets a few pings to settle and the player has time to react

// Slower than before on purpose: this both eases the CPU/I2C/ultrasonic
// workload and, since the dart chance above is rolled once per tick, makes
// darts roll (and so happen) less often in real time too.
#define SAMPLE_INTERVAL_MS 200

MFRC522 rfid(SS_PIN, RST_PIN);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);

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
uint32_t lastOledMs = 0;
uint32_t fryStartMs = 0;

// Maps a smoothed 0..1 position (handPos/targetPos) to a screen column.
// Left = near the sensor (pos 1.0), right = far from it (pos 0.0), per the
// physical near/far axis the ultrasonic measures.
int posToScreenX(float pos) {
  pos = constrain(pos, 0.0f, 1.0f);
  return (int)roundf((1.0f - pos) * (OLED_WIDTH - 1));
}

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

// Fry progress bar, filled left-to-right. Ramps amber -> green so a glance
// tells you roughly how close to done you are, not just the raw fraction.
void renderProgressBar() {
  strip.clear();

  int litPixels = (int)roundf(fryProgress * LED_COUNT);
  for (int i = 0; i < litPixels; i++) {
    uint8_t g = (uint8_t)(80 + fryProgress * 175);  // 80 -> 255
    uint8_t r = (uint8_t)(150 * (1.0f - fryProgress)); // 150 -> 0
    strip.setPixelColor(i, strip.Color(r, g, 0));
  }

  strip.show();
}

// Target zone bar: top band, y=2..13.
#define TRACKER_TARGET_Y      2
#define TRACKER_TARGET_HEIGHT 12

// Hand marker box: bottom band, y=18..29.
#define TRACKER_HAND_Y      18
#define TRACKER_HAND_HEIGHT 12
#define TRACKER_HAND_WIDTH  10

// OLED tracker: a solid bar shows the target zone's currently-acceptable
// range, a box shows the hand's actual measured position, both on the same
// left=near / right=far axis so lining the box up under the bar means
// you're in the zone. The panel is monochrome, so "in the zone" is shown
// by the box switching from hollow to solid (and by the two lining up
// vertically), not by color. Throttled separately from the LED strip
// since a full-frame I2C push is slower than a NeoPixel update.
void renderTrackerOled(bool overlap) {
  uint32_t now = millis();
  if (now - lastOledMs < OLED_UPDATE_INTERVAL_MS) return;
  lastOledMs = now;

  display.clearDisplay();

  // Target zone bar spans the actual catch-zone width, in screen space.
  // Screen X is flipped relative to pos (left = near = pos 1.0), so the
  // "low" edge in pos-space lands on the right in screen-space.
  int targetXLo = posToScreenX(targetPos + CATCH_ZONE_FRAC / 2);
  int targetXHi = posToScreenX(targetPos - CATCH_ZONE_FRAC / 2);
  targetXLo = constrain(targetXLo, 0, OLED_WIDTH - 1);
  targetXHi = constrain(targetXHi, 0, OLED_WIDTH - 1);
  display.fillRect(targetXLo, TRACKER_TARGET_Y, targetXHi - targetXLo + 1,
                    TRACKER_TARGET_HEIGHT, SSD1306_WHITE);

  // Hand marker box, filled solid when it's inside the target zone.
  int handX = posToScreenX(handPos);
  int handXLo = constrain(handX - TRACKER_HAND_WIDTH / 2, 0, OLED_WIDTH - TRACKER_HAND_WIDTH);
  if (overlap) {
    display.fillRect(handXLo, TRACKER_HAND_Y, TRACKER_HAND_WIDTH, TRACKER_HAND_HEIGHT, SSD1306_WHITE);
  } else {
    display.drawRect(handXLo, TRACKER_HAND_Y, TRACKER_HAND_WIDTH, TRACKER_HAND_HEIGHT, SSD1306_WHITE);
  }

  display.display();
}

void flashResult(bool success) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 8);
  display.print(success ? "PERFECT!" : "BURNT!");
  display.display();

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

void showIdleScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 12);
  display.print("Scan an item to fry");
  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  strip.begin();
  strip.clear();
  strip.show();

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED init failed");
  } else {
    showIdleScreen();
  }

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
      fryProgress = FRY_START_PROGRESS;
      lastSampleMs = millis();
      lastPingMs = 0;
      fryStartMs = millis();
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

      bool inGrace = (now - fryStartMs < FRY_GRACE_MS);
      if (!inGrace) {
        fryProgress += (overlap ? PROGRESS_FILL_PER_SEC : -PROGRESS_DRAIN_PER_SEC) * dt;
        fryProgress = constrain(fryProgress, 0.0f, 1.0f);
      }

      renderProgressBar();
      renderTrackerOled(overlap);

      if (!inGrace) {
        if (fryProgress >= 1.0f) state = DONE_SUCCESS;
        else if (fryProgress <= 0.0f) state = DONE_FAIL;
      }
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
      showIdleScreen();
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
      showIdleScreen();
      state = IDLE;
      break;
    }
  }
}
