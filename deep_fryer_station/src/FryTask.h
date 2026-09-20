#pragma once
// Deep fryer task: mimics Stardew Valley's fishing minigame. While a tag is
// active, keep your hand's height over the HC-SR04 aligned with a roaming
// target zone (the "fish") to fill fryer::ProgressTracker's progress bar
// before it drains back to zero; reaching the server's goal finishes the
// task, same contract as PressTask / JoystickPatternTask.
//
// The OLED draws the target zone as a bar and the hand as a box on the same
// near/far axis (left = near the sensor, right = far); the shared LED strip
// already draws the task's progress bar and the success flash
// (StationCore's Display, driven by Station.cpp), so this task never touches
// the strip directly.

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <StationTask.h>
#include <Wire.h>
#include <math.h>

#include "FryTracker.h"

class FryTask : public station::StationTask {
 public:
  // trigPin/echoPin: HC-SR04 (echo through a 5V->3.3V divider, see main.cpp).
  // sdaPin/sclPin/oledAddress: SSD1306 OLED, 128x32, I2C.
  // nearCm/farCm: hand-height range to track, tune to the physical mount.
  // catchZoneFrac: how much of that range counts as a hit (0-1); bigger = easier.
  FryTask(uint8_t trigPin, uint8_t echoPin, uint8_t sdaPin, uint8_t sclPin, uint8_t oledAddress = 0x3C,
          float nearCm = 3.0f, float farCm = 30.0f, float catchZoneFrac = 0.30f)
      : trigPin_(trigPin), echoPin_(echoPin), sdaPin_(sdaPin), sclPin_(sclPin), oledAddress_(oledAddress),
        nearCm_(nearCm), farCm_(farCm), catchZoneFrac_(catchZoneFrac),
        display_(kOledWidth, kOledHeight, &Wire, -1) {}

  oc::TaskKind kind() const override { return oc::TaskKind::Fry; }

  void begin() override {
    pinMode(trigPin_, OUTPUT);
    pinMode(echoPin_, INPUT);
    digitalWrite(trigPin_, LOW);

    Wire.begin(sdaPin_, sclPin_);
    oledOk_ = display_.begin(SSD1306_SWITCHCAPVCC, oledAddress_);
    if (!oledOk_) {
      Serial.println("Fryer OLED not answering, check the wiring/address");
    } else {
      showIdle();
    }

    randomSeed(esp_random());
  }

  void start(uint16_t goal, uint16_t progress, uint8_t) override {
    goal_ = goal ? goal : 1;
    tracker_.reset(progress ? (float)progress / goal_ : fryer::kStartProgress);
    doneShown_ = false;

    handPos_ = targetPos_ = 0.5f;
    targetVel_ = 0.0f;
    darting_ = false;

    uint32_t now = millis();
    graceUntilMs_ = now + kGraceMs;
    lastUpdateMs_ = now;
    lastPingMs_ = 0;
    active_ = true;
  }

  void stop() override {
    active_ = false;
    showIdle();
  }

  void update(uint32_t now) override {
    if (!active_) return;

    if (now - lastPingMs_ >= kPingIntervalMs) {
      lastPingMs_ = now;
      updateHandPos();
    }

    if (tracker_.done()) {
      if (!doneShown_) {
        showDone();
        doneShown_ = true;
      }
      return;
    }

    if (now - lastUpdateMs_ < kSampleIntervalMs) return;
    float dt = (now - lastUpdateMs_) / 1000.0f;
    lastUpdateMs_ = now;

    updateTarget(dt, now);
    bool overlap = fryer::overlaps(handPos_, targetPos_, catchZoneFrac_);
    tracker_.update(overlap, dt, now < graceUntilMs_);
    renderTracker(overlap);
  }

  uint16_t progress() const override { return (uint16_t)roundf(tracker_.progress() * goal_); }
  bool done() const override { return active_ && tracker_.done(); }

 private:
  static constexpr int kOledWidth = 128;
  static constexpr int kOledHeight = 32;

  static constexpr uint32_t kPingIntervalMs = 60;    // HC-SR04 needs quiet time for its echo to die out
  static constexpr uint32_t kSampleIntervalMs = 200; // target motion / scoring tick
  static constexpr uint32_t kGraceMs = 750;           // scoring paused this long after start(), so the ultrasonic settles

  // The sensor only ever needs to see NEAR_CM..FAR_CM (a few tens of cm), so
  // the ping timeout is sized to that, not the sensor's full ~5m range -
  // pulseIn() blocks until it returns, and StationTask::update() must not
  // block for long.
  static constexpr unsigned long kPingTimeoutUs = 6000;
  static constexpr float kHandSmoothing = 0.35f; // exponential smoothing, lower = smoother/slower

  // Target ("fish") motion: mostly drifts, occasionally darts.
  static constexpr float kDriftAccel = 0.4f;        // fraction/sec^2, pull toward a wandering velocity
  static constexpr float kMaxDriftSpeed = 0.3f;     // fraction/sec, normal drift speed
  static constexpr int kDartChancePer10k = 100;     // checked once per sample tick
  static constexpr float kDartSpeed = 1.0f;         // fraction/sec, during a dart
  static constexpr uint32_t kDartMs = 200;

  static constexpr int kTargetY = 2, kTargetHeight = 12;              // top band
  static constexpr int kHandY = 18, kHandHeight = 12, kHandWidth = 10; // bottom band

  // Fires one ping and returns distance in cm, or -1 if no echo came back in time.
  float pingDistanceCm() {
    digitalWrite(trigPin_, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin_, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin_, LOW);

    unsigned long durationUs = pulseIn(echoPin_, HIGH, kPingTimeoutUs);
    if (durationUs == 0) return -1;
    return durationUs / 58.0f; // standard HC-SR04 us-to-cm conversion
  }

  // Converts a raw distance into a smoothed 0..1 hand position, closer =
  // higher. A missed ping just skips the update instead of snapping to an endpoint.
  void updateHandPos() {
    float distanceCm = pingDistanceCm();
    if (distanceCm < 0) return;
    float raw = constrain((farCm_ - distanceCm) / (farCm_ - nearCm_), 0.0f, 1.0f);
    handPos_ += (raw - handPos_) * kHandSmoothing;
  }

  // Advances the roaming target by dt seconds: a smooth random walk that
  // occasionally darts fast in a random direction, like a fish that mostly
  // drifts but sometimes bolts. Bounces off the ends instead of clamping.
  void updateTarget(float dt, uint32_t now) {
    if (darting_ && now >= dartEndsAtMs_) darting_ = false;

    if (!darting_ && random(0, 10000) < kDartChancePer10k) {
      darting_ = true;
      dartEndsAtMs_ = now + kDartMs;
      targetVel_ = (random(0, 2) == 0 ? -1.0f : 1.0f) * kDartSpeed;
    } else if (!darting_) {
      float wanderTo = (random(-1000, 1001) / 1000.0f) * kMaxDriftSpeed;
      float accel = kDriftAccel * dt;
      if (targetVel_ < wanderTo) targetVel_ = min(targetVel_ + accel, wanderTo);
      else targetVel_ = max(targetVel_ - accel, wanderTo);
    }

    targetPos_ += targetVel_ * dt;
    if (targetPos_ < 0.0f) { targetPos_ = -targetPos_; targetVel_ = -targetVel_; }
    if (targetPos_ > 1.0f) { targetPos_ = 2.0f - targetPos_; targetVel_ = -targetVel_; }
  }

  // Left = near the sensor (pos 1.0), right = far (pos 0.0).
  int posToScreenX(float pos) const {
    pos = constrain(pos, 0.0f, 1.0f);
    return (int)roundf((1.0f - pos) * (kOledWidth - 1));
  }

  // A solid bar shows the target zone's current range, a box shows the
  // hand's measured position, both on the near/far axis - lining the box up
  // under the bar means the hand is in the zone. Monochrome, so "in the
  // zone" is the box switching from hollow to solid, not a color change.
  void renderTracker(bool overlap) {
    if (!oledOk_) return;
    display_.clearDisplay();

    int targetXLo = constrain(posToScreenX(targetPos_ + catchZoneFrac_ / 2), 0, kOledWidth - 1);
    int targetXHi = constrain(posToScreenX(targetPos_ - catchZoneFrac_ / 2), 0, kOledWidth - 1);
    display_.fillRect(targetXLo, kTargetY, targetXHi - targetXLo + 1, kTargetHeight, SSD1306_WHITE);

    int handX = posToScreenX(handPos_);
    int handXLo = constrain(handX - kHandWidth / 2, 0, kOledWidth - kHandWidth);
    if (overlap) {
      display_.fillRect(handXLo, kHandY, kHandWidth, kHandHeight, SSD1306_WHITE);
    } else {
      display_.drawRect(handXLo, kHandY, kHandWidth, kHandHeight, SSD1306_WHITE);
    }

    display_.display();
  }

  void showIdle() {
    if (!oledOk_) return;
    display_.clearDisplay();
    display_.setTextSize(1);
    display_.setTextColor(SSD1306_WHITE);
    display_.setCursor(0, 12);
    display_.print("Scan an item to fry");
    display_.display();
  }

  void showDone() {
    if (!oledOk_) return;
    display_.clearDisplay();
    display_.setTextSize(2);
    display_.setTextColor(SSD1306_WHITE);
    display_.setCursor(0, 8);
    display_.print("FRIED!");
    display_.display();
  }

  uint8_t trigPin_, echoPin_, sdaPin_, sclPin_, oledAddress_;
  float nearCm_, farCm_, catchZoneFrac_;
  Adafruit_SSD1306 display_;
  bool oledOk_ = false;

  uint16_t goal_ = 1;
  fryer::ProgressTracker tracker_;
  bool active_ = false;
  bool doneShown_ = false;

  float handPos_ = 0.5f;   // smoothed 0..1, 0 = farCm_, 1 = nearCm_
  float targetPos_ = 0.5f; // 0..1, the roaming "fish"
  float targetVel_ = 0.0f; // fraction of range per second
  bool darting_ = false;
  uint32_t dartEndsAtMs_ = 0;

  uint32_t lastUpdateMs_ = 0;
  uint32_t lastPingMs_ = 0;
  uint32_t graceUntilMs_ = 0;
};
