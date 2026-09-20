#pragma once
// Sink task: scrub an analog joystick in circles until the goal (milliseconds
// of active scrubbing, set by the server) is reached. Progress and goal are in
// milliseconds, so the shared LED bar fills as the plate gets clean.
//
// Two analog axes, read on ADC1 pins (ADC2 stops working while WiFi is on, and
// ESP-NOW needs WiFi). GPIO34 and GPIO35 are input-only pins on the left header,
// which is fine for analog inputs.
// Power the joystick from 3V3, NOT 5V: at 5V its outputs can exceed what the ESP32 pins tolerate.

#include <StationTask.h>

#include "ScrubTracker.h"

class ScrubTask : public station::StationTask {
 public:
  ScrubTask(uint8_t xPin, uint8_t yPin) : xPin_(xPin), yPin_(yPin) {}

  oc::TaskKind kind() const override { return oc::TaskKind::Scrub; }

  // The stick's resting position is measured here, so keep it untouched while booting.
  void begin() override { centre(); }

  // ... and again whenever a plate arrives: the stick is assumed to be at rest right now.
  void start(uint16_t goal, uint16_t progress, uint8_t) override {
    goal_ = goal ? goal : 1;
    tracker_.reset(progress);
    centre();
    lastSampleMs_ = millis();
    active_ = true;
  }

  void stop() override { active_ = false; }

  void update(uint32_t now) override {
    if (!active_ || tracker_.activeMs() >= goal_) return;
    if (now - lastSampleMs_ < kSampleIntervalMs) return;
    uint32_t dt = now - lastSampleMs_;
    lastSampleMs_ = now;
    tracker_.update(analogRead(xPin_) - centreX_, analogRead(yPin_) - centreY_, dt);
  }

  uint16_t progress() const override {
    uint32_t ms = tracker_.activeMs();
    return ms < goal_ ? (uint16_t)ms : goal_;
  }
  bool done() const override { return active_ && tracker_.activeMs() >= goal_; }

 private:
  static constexpr uint32_t kSampleIntervalMs = 20;

  // Average a handful of samples so the pot does not have to sit at exactly 2048.
  void centre() {
    constexpr int kSamples = 20;
    long sumX = 0, sumY = 0;
    for (int i = 0; i < kSamples; i++) {
      sumX += analogRead(xPin_);
      sumY += analogRead(yPin_);
      delay(2);
    }
    centreX_ = sumX / (float)kSamples;
    centreY_ = sumY / (float)kSamples;
    Serial.printf("Joystick centre: %.0f, %.0f\n", centreX_, centreY_);
  }

  uint8_t xPin_, yPin_;
  float centreX_ = 2048, centreY_ = 2048;
  uint16_t goal_ = 1;
  bool active_ = false;
  uint32_t lastSampleMs_ = 0;
  scrub::ScrubTracker tracker_;
};
