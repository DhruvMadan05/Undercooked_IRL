#include "Display.h"

namespace station {
namespace {

using oc::DisplayMode;

struct FlashSpec {
  uint8_t r, g, b;
  uint8_t flashes;
  uint16_t stepMs; // length of each on and each off half
};

FlashSpec flashSpec(DisplayMode mode) {
  switch (mode) {
    case DisplayMode::Calibrated: return {0, 255, 0, 2, 250};
    case DisplayMode::Success:    return {0, 255, 0, 5, 250};
    case DisplayMode::Reject:     return {255, 0, 0, 3, 150};
    default:                      return {0, 0, 0, 0, 1};
  }
}

bool isFlash(DisplayMode mode) {
  return mode == DisplayMode::Calibrated || mode == DisplayMode::Success ||
         mode == DisplayMode::Reject;
}

struct Rgb {
  uint8_t r, g, b;
};

// One colour per oc::Pattern, so the pan's Simon Says can say "do this one"
// on a bare LED strip. Kept away from the orange/red family the cooking,
// warning and burnt modes use.
Rgb patternColor(uint8_t pattern) {
  switch (pattern) {
    case (uint8_t)oc::Pattern::Circle: return {0, 0, 255};     // blue
    case (uint8_t)oc::Pattern::Zigzag: return {255, 255, 0};   // yellow
    case (uint8_t)oc::Pattern::Hold:   return {160, 0, 255};   // purple
    case (uint8_t)oc::Pattern::Shake:  return {255, 0, 150};   // pink
    case (uint8_t)oc::Pattern::Press:  return {255, 255, 255}; // white
    case (uint8_t)oc::Pattern::Flick:  return {0, 255, 255};   // cyan
    default:                           return {0, 0, 0};
  }
}

constexpr uint8_t kPatternCount = 6;

} // namespace

void Display::begin() {
  strip_.begin();
  strip_.clear();
  strip_.show();
}

uint32_t Display::color(uint8_t r, uint8_t g, uint8_t b) const {
  return strip_.Color((uint16_t)r * brightness_ / 255, (uint16_t)g * brightness_ / 255,
                      (uint16_t)b * brightness_ / 255);
}

// Pixels to light for value/goal, rounding up so any progress shows.
uint8_t Display::litFor(uint32_t value, uint32_t goal, uint8_t count) const {
  if (goal == 0 || value == 0) return 0;
  if (value >= goal) return count;
  return (value * count + goal - 1) / goal;
}

void Display::setMode(DisplayMode mode, uint8_t level) {
  if (isFlash(mode)) {
    flash(mode);
    return;
  }
  base_ = mode;
  baseLevel_ = level;
}

void Display::flash(DisplayMode mode) {
  if (!isFlash(mode)) return;
  flashing_ = true;
  flashMode_ = mode;
  flashStartedAt_ = millis();
}

void Display::setProgress(uint16_t value, uint16_t goal) {
  progressShown_ = true;
  progress_ = value;
  goal_ = goal;
}

void Display::clearProgress() {
  progressShown_ = false;
}

void Display::update(uint32_t now) {
  uint8_t n = strip_.numPixels();
  if (n > kMaxPixels) n = kMaxPixels;
  uint32_t frame[kMaxPixels] = {};

  if (offline_) {
    if ((now / 500) % 2 == 0) frame[0] = color(255, 0, 0);
  } else if (flashing_) {
    FlashSpec spec = flashSpec(flashMode_);
    uint32_t step = (now - flashStartedAt_) / spec.stepMs;
    if (step >= (uint32_t)spec.flashes * 2) {
      flashing_ = false;
    } else if (step % 2 == 0) {
      for (uint8_t i = 0; i < n; i++) frame[i] = color(spec.r, spec.g, spec.b);
    }
  }

  if (!offline_ && !flashing_) {
    switch (base_) {
      case DisplayMode::Idle:
        if (progressShown_) {
          uint8_t lit = litFor(progress_, goal_, n);
          for (uint8_t i = 0; i < lit; i++) frame[i] = color(0, 0, 255);
        }
        break;
      case DisplayMode::Cooking: {
        uint8_t lit = litFor(baseLevel_, 255, n);
        if (lit == 0) lit = 1;
        for (uint8_t i = 0; i < lit; i++) frame[i] = color(255, 100, 0);
        break;
      }
      case DisplayMode::Warning:
        if ((now / 250) % 2 == 0) {
          for (uint8_t i = 0; i < n; i++) frame[i] = color(255, 60, 0);
        }
        break;
      case DisplayMode::Burnt:
        for (uint8_t i = 0; i < n; i++) frame[i] = color(255, 0, 0);
        break;
      case DisplayMode::GameOver:
        for (uint8_t i = 0; i < n; i++) frame[i] = color(255, 255, 255);
        break;
      case DisplayMode::PatternCue: {
        bool lowTime = baseLevel_ >= kPatternCount;
        if (!lowTime || (now / 200) % 2 == 0) {
          Rgb c = patternColor(baseLevel_ % kPatternCount);
          for (uint8_t i = 0; i < n; i++) frame[i] = color(c.r, c.g, c.b);
        }
        break;
      }
      default:
        break;
    }
  }

  if (lastValid_ && memcmp(frame, lastFrame_, sizeof(uint32_t) * n) == 0) return;
  for (uint8_t i = 0; i < n; i++) strip_.setPixelColor(i, frame[i]);
  strip_.show();
  memcpy(lastFrame_, frame, sizeof(uint32_t) * n);
  lastValid_ = true;
}

} // namespace station
