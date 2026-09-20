#pragma once
// LED strip for one station. Everything is non-blocking: call update() every
// loop and it redraws (only when the picture changed).
//
// What is on the strip, highest priority first:
//   1. offline    - server not answering: one red pixel blinks slowly
//   2. a flash    - one-shot Calibrated / Success / Reject, then back to 3.
//   3. the base   - the last persistent mode from the server (Idle, Burnt,
//                   GameOver, PlateClean, PlateDirty, PatternCue). Idle shows the
//                   task's progress bar while a task is running, and nothing
//                   otherwise. PatternCue fills the strip in the colour of the
//                   joystick pattern to do now (see patternColor in Display.cpp).

#include <Adafruit_NeoPixel.h>
#include <OvercookedComm.h>

#include "DisplaySink.h"

namespace station {

class Display : public DisplaySink {
 public:
  // brightness: 0-255, applied to every colour.
  explicit Display(Adafruit_NeoPixel &strip, uint8_t brightness = 80)
      : strip_(strip), brightness_(brightness) {}

  void begin() override;
  void update(uint32_t now) override;

  // From the server. Calibrated / Success / Reject flash once; the rest stay
  // until the next setMode. level is used by PatternCue (pattern id, +4 to blink).
  void setMode(oc::DisplayMode mode, uint8_t level) override;

  // Local flash, e.g. celebrating a finished task without waiting for the server.
  void flash(oc::DisplayMode mode) override;

  // Task progress bar: value out of goal. clearProgress() hides it.
  void setProgress(uint16_t value, uint16_t goal) override;
  void clearProgress() override;

  void setOffline(bool offline) override { offline_ = offline; }

 private:
  static constexpr uint8_t kMaxPixels = 16;

  uint32_t color(uint8_t r, uint8_t g, uint8_t b) const;
  uint8_t litFor(uint32_t value, uint32_t goal, uint8_t count) const;

  Adafruit_NeoPixel &strip_;
  uint8_t brightness_;

  bool offline_ = false;

  oc::DisplayMode base_ = oc::DisplayMode::Idle;
  uint8_t baseLevel_ = 0;

  bool progressShown_ = false;
  uint16_t progress_ = 0;
  uint16_t goal_ = 1;

  bool flashing_ = false;
  oc::DisplayMode flashMode_ = oc::DisplayMode::Idle;
  uint32_t flashStartedAt_ = 0;

  uint32_t lastFrame_[kMaxPixels] = {};
  bool lastValid_ = false;
};

} // namespace station
