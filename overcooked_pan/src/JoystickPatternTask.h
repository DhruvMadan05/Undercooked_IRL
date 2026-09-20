#pragma once
// Frying pan task: move an analog joystick (and its click button) in the
// pattern the server asks for (Accept.param, an oc::Pattern) until the goal
// number of steps is reached.
//
// Two analog axes, read on ADC1 pins (ADC2 stops working while WiFi is on, and
// ESP-NOW needs WiFi). GPIO34 and GPIO35 are input-only pins on the left header,
// which is fine for analog inputs.
// Power the joystick from 3V3, NOT 5V: at 5V its outputs can exceed what the ESP32 pins tolerate.
// The click pin uses the ESP32's internal pull-up (reads HIGH released, LOW
// pressed), same as the cutting board's limit switch - no external resistor.

#include <StationTask.h>

#include "PatternTracker.h"

static_assert((uint8_t)oc::Pattern::Circle == pan::kCircle, "pattern ids out of step with OvercookedComm.h");
static_assert((uint8_t)oc::Pattern::Zigzag == pan::kZigzag, "pattern ids out of step with OvercookedComm.h");
static_assert((uint8_t)oc::Pattern::Hold == pan::kHold, "pattern ids out of step with OvercookedComm.h");
static_assert((uint8_t)oc::Pattern::Shake == pan::kShake, "pattern ids out of step with OvercookedComm.h");
static_assert((uint8_t)oc::Pattern::Press == pan::kPress, "pattern ids out of step with OvercookedComm.h");
static_assert((uint8_t)oc::Pattern::Flick == pan::kFlick, "pattern ids out of step with OvercookedComm.h");

class JoystickPatternTask : public station::StationTask {
 public:
  // deadZone: how far the stick must be pushed (0-1 of full travel) to count
  // as a direction. invertX / invertY flip an axis that reads backwards
  // for how the joystick is mounted (up must read positive).
  JoystickPatternTask(uint8_t xPin, uint8_t yPin, uint8_t clickPin, float deadZone = 0.6f,
                      bool invertX = false, bool invertY = false)
      : xPin_(xPin), yPin_(yPin), clickPin_(clickPin), deadZone_(deadZone), invertX_(invertX),
        invertY_(invertY) {}

  oc::TaskKind kind() const override { return oc::TaskKind::JoystickPattern; }

  // The stick's resting position is measured here, so keep it untouched while booting.
  void begin() override {
    pinMode(clickPin_, INPUT_PULLUP);

    uint32_t sumX = 0, sumY = 0;
    constexpr int kSamples = 32;
    for (int i = 0; i < kSamples; i++) {
      sumX += analogRead(xPin_);
      sumY += analogRead(yPin_);
      delay(2);
    }
    centerX_ = sumX / kSamples;
    centerY_ = sumY / kSamples;
    smoothX_ = centerX_;
    smoothY_ = centerY_;
    Serial.printf("Joystick centre: %.0f, %.0f\n", centerX_, centerY_);
  }

  void start(uint16_t goal, uint16_t progress, uint8_t param) override {
    goal_ = goal;
    count_ = progress;
    tracker_.reset(param);
    active_ = true;
  }

  void stop() override { active_ = false; }

  void update(uint32_t now) override {
    // A light low-pass filter so ADC noise does not flicker across a direction boundary.
    smoothX_ += 0.3f * (analogRead(xPin_) - smoothX_);
    smoothY_ += 0.3f * (analogRead(yPin_) - smoothY_);

    // Debounced click, same falling-edge-is-pressed logic as the cutting
    // board's PressTask; runs every update so it stays current even when idle.
    bool reading = digitalRead(clickPin_);
    if (reading != lastClickRead_) {
      lastClickRead_ = reading;
      clickChangedAt_ = now;
    }
    if (now - clickChangedAt_ >= kClickDebounceMs) {
      clickStable_ = reading;
    }

    if (!active_ || count_ >= goal_) return;

    float x = normalise(smoothX_, centerX_, invertX_);
    float y = normalise(smoothY_, centerY_, invertY_);
    bool clicked = clickStable_ == LOW;
    if (tracker_.update(pan::quantize(x, y, deadZone_), clicked, now)) {
      count_++;
      Serial.printf("Pattern step %u/%u\n", count_, goal_);
    }
  }

  uint16_t progress() const override { return count_; }
  bool done() const override { return active_ && count_ >= goal_; }

 private:
  // -1..1 around the resting position. The ADC is 12 bit, so full travel is about 2048 each way.
  static float normalise(float raw, float center, bool invert) {
    float v = (raw - center) / 2048.0f;
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    return invert ? -v : v;
  }

  static constexpr uint32_t kClickDebounceMs = 20;

  uint8_t xPin_, yPin_, clickPin_;
  float deadZone_;
  bool invertX_, invertY_;
  float centerX_ = 2048, centerY_ = 2048;
  float smoothX_ = 2048, smoothY_ = 2048;
  uint16_t goal_ = 0;
  uint16_t count_ = 0;
  bool active_ = false;
  pan::PatternTracker tracker_;

  bool lastClickRead_ = HIGH;   // raw state from the previous update()
  bool clickStable_ = HIGH;     // debounced state (HIGH = released)
  uint32_t clickChangedAt_ = 0;
};
