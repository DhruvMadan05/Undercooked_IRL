#pragma once
// Cutting board task: count presses of a limit switch.
//   Switch COM -> GPIO14
//   Switch NO  -> GND
// Uses the ESP32's internal pull-up, so no external resistor is needed: the
// pin reads HIGH when open and LOW when the switch is pressed. Use the NO
// (normally open) terminal, not NC, or the count will trigger on release.

#include <StationTask.h>

class PressTask : public station::StationTask {
 public:
  PressTask(uint8_t pin, uint16_t debounceMs) : pin_(pin), debounceMs_(debounceMs) {}

  oc::TaskKind kind() const override { return oc::TaskKind::Presses; }

  void begin() override { pinMode(pin_, INPUT_PULLUP); }

  void start(uint16_t goal, uint16_t progress, uint8_t) override {
    goal_ = goal;
    count_ = progress;
    active_ = true;
  }

  void stop() override { active_ = false; }

  // Debounced falling-edge detection: counts once per press, not on release.
  // Runs even when no task is active so the switch state stays current.
  void update(uint32_t now) override {
    bool reading = digitalRead(pin_);
    if (reading != lastRead_) {
      lastRead_ = reading;
      changedAt_ = now;
    }
    if (reading != stable_ && (now - changedAt_) >= debounceMs_) {
      stable_ = reading;
      if (stable_ == LOW && active_ && count_ < goal_) {
        count_++;
        Serial.printf("Press %u/%u\n", count_, goal_);
      }
    }
  }

  uint16_t progress() const override { return count_; }
  bool done() const override { return active_ && count_ >= goal_; }

 private:
  uint8_t pin_;
  uint16_t debounceMs_;
  uint16_t goal_ = 0;
  uint16_t count_ = 0;
  bool active_ = false;
  bool stable_ = HIGH;   // debounced state (HIGH = released)
  bool lastRead_ = HIGH; // raw state from the previous loop
  uint32_t changedAt_ = 0;
};
