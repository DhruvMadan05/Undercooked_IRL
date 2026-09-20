#pragma once
// Turns joystick positions (and the click button) into "pattern steps". Pure
// logic (no Arduino, no hardware) so it can be unit tested on the host.
//
// The stick is reduced to one of four directions (or none, in the dead zone).
// Feed every reading to update(), with the click button's debounced state and
// the current time in ms (free-running, e.g. millis() - only Hold, Shake,
// Press and Flick care about it); it returns true each time the player makes
// the next move of the pattern:
//   Circle  sweep round the edge. The first direction counts, the second one
//           picks clockwise or counter-clockwise, after which each step has to
//           be the next direction in that same rotation. Wobbling back and
//           forth, or jumping across the stick, does not count.
//   Zigzag  swing left, right, left, ... only Left and Right count and each one
//           has to be the opposite of the last.
//   Hold    keep the stick centered. Counts one step every kHoldStepMs of
//           unbroken stillness; leaving center resets the dwell.
//   Shake   like Zigzag but on either axis, and fast: each alternation only
//           counts if it lands within kShakeWindowMs of the previous one.
//   Press   hold the click button down for kPressHoldMs; release it before
//           the next press can count.
//   Flick   push the stick up while holding the click, the two landing within
//           kFlickWindowMs of each other; release either before the next one.
// The numbers match oc::Pattern in OvercookedComm.h (JoystickPatternTask.h
// checks that at compile time).

#include <math.h>
#include <stdint.h>

namespace pan {

constexpr uint8_t kCircle = 0;
constexpr uint8_t kZigzag = 1;
constexpr uint8_t kHold = 2;
constexpr uint8_t kShake = 3;
constexpr uint8_t kPress = 4;
constexpr uint8_t kFlick = 5;

constexpr uint32_t kHoldStepMs = 500;    // unbroken center dwell per Hold step
constexpr uint32_t kShakeWindowMs = 400; // max gap between alternations for Shake
constexpr uint32_t kPressHoldMs = 250;   // click must stay down this long for Press
constexpr uint32_t kFlickWindowMs = 200; // max gap between the click and the Up push for Flick

enum class Direction : int8_t { None = -1, Up = 0, Right = 1, Down = 2, Left = 3 };

// x: right is positive, y: up is positive, both -1..1. Inside the dead zone
// (radius deadZone) there is no direction.
inline Direction quantize(float x, float y, float deadZone) {
  if (x * x + y * y < deadZone * deadZone) return Direction::None;
  if (fabsf(y) > fabsf(x)) return y > 0 ? Direction::Up : Direction::Down;
  return x > 0 ? Direction::Right : Direction::Left;
}

inline bool isOpposite(Direction a, Direction b) {
  return (a == Direction::Left && b == Direction::Right) || (a == Direction::Right && b == Direction::Left) ||
         (a == Direction::Up && b == Direction::Down) || (a == Direction::Down && b == Direction::Up);
}

class PatternTracker {
 public:
  // Forget where the player was, and switch to a pattern.
  void reset(uint8_t pattern) {
    pattern_ = pattern;
    last_ = Direction::None;
    spin_ = 0;
    holding_ = false;
    lastEdgeMs_ = 0;
    pressing_ = false;
    pressFired_ = false;
    pressStartMs_ = 0;
    wasUp_ = false;
    upEdgeMs_ = 0;
    clickHeld_ = false;
    clickEdgeMs_ = 0;
    flickFired_ = false;
  }

  // True when this reading completes one more step of the pattern. clicked
  // only matters to Press and Flick; nowMs only matters to Hold, Shake, Press
  // and Flick.
  bool update(Direction d, bool clicked, uint32_t nowMs) {
    if (pattern_ == kHold) return holdStep(d, nowMs);
    if (pattern_ == kPress) return pressStep(clicked, nowMs);
    if (pattern_ == kFlick) return flickStep(d, clicked, nowMs);
    if (d == Direction::None || d == last_) return false;

    // Shake tracks the current direction on every edge (even ones that don't
    // count, e.g. a too-slow reversal), unlike Circle/Zigzag below which only
    // advance their reference point on a counted step - so a slow reversal
    // still sets up the next one to be judged against, instead of a stuck
    // "no prior direction" that could never start counting.
    if (pattern_ == kShake) return shakeStep(d, nowMs);

    bool counts = pattern_ == kZigzag ? zigzagStep(d) : circleStep(d);
    if (counts) last_ = d;
    return counts;
  }

 private:
  bool zigzagStep(Direction d) const {
    return d == Direction::Left || d == Direction::Right; // d != last_ was checked
  }

  bool circleStep(Direction d) {
    if (last_ == Direction::None) return true;
    int step = ((int)d - (int)last_ + 4) % 4; // 1 = clockwise neighbour, 3 = counter-clockwise
    if (step != 1 && step != 3) return false;
    int rotation = step == 1 ? 1 : -1;
    if (spin_ == 0) spin_ = rotation;
    return rotation == spin_;
  }

  // Only counts an edge that reverses the previous direction (either axis)
  // and arrives within kShakeWindowMs of it - a slow alternation is just a
  // lazy Zigzag, not a Shake. last_ advances on every edge regardless, so a
  // slow (non-counting) reversal still becomes the reference for the next one.
  bool shakeStep(Direction d, uint32_t nowMs) {
    bool counts = last_ != Direction::None && isOpposite(d, last_) && (nowMs - lastEdgeMs_) <= kShakeWindowMs;
    last_ = d;
    lastEdgeMs_ = nowMs;
    return counts;
  }

  // Every kHoldStepMs of unbroken time at center counts one step; moving off
  // center at all aborts the current dwell (it does not just pause it).
  bool holdStep(Direction d, uint32_t nowMs) {
    if (d != Direction::None) {
      holding_ = false;
      return false;
    }
    if (!holding_) {
      holding_ = true;
      lastEdgeMs_ = nowMs;
      return false;
    }
    if (nowMs - lastEdgeMs_ >= kHoldStepMs) {
      lastEdgeMs_ = nowMs;
      return true;
    }
    return false;
  }

  // Counts once when the click has been held continuously for kPressHoldMs;
  // stays quiet (not just paused) until the click is released, so holding it
  // down doesn't repeat-fire.
  bool pressStep(bool clicked, uint32_t nowMs) {
    if (!clicked) {
      pressing_ = false;
      pressFired_ = false;
      return false;
    }
    if (!pressing_) {
      pressing_ = true;
      pressStartMs_ = nowMs;
      return false;
    }
    if (pressFired_ || nowMs - pressStartMs_ < kPressHoldMs) return false;
    pressFired_ = true;
    return true;
  }

  // Counts when the click and an Up push are both currently active and their
  // onsets landed within kFlickWindowMs of each other - a deliberate click
  // snapped together with the stick, not an independent press and a later
  // push. Rearms once either one lets go.
  bool flickStep(Direction d, bool clicked, uint32_t nowMs) {
    bool upNow = d == Direction::Up;
    if (upNow && !wasUp_) upEdgeMs_ = nowMs;
    wasUp_ = upNow;
    if (clicked && !clickHeld_) clickEdgeMs_ = nowMs;
    clickHeld_ = clicked;

    if (!clicked || !upNow) {
      flickFired_ = false;
      return false;
    }
    if (flickFired_) return false;
    uint32_t gap = clickEdgeMs_ > upEdgeMs_ ? clickEdgeMs_ - upEdgeMs_ : upEdgeMs_ - clickEdgeMs_;
    if (gap > kFlickWindowMs) return false;
    flickFired_ = true;
    return true;
  }

  uint8_t pattern_ = kCircle;
  Direction last_ = Direction::None;
  int8_t spin_ = 0;         // circle: +1 clockwise, -1 counter-clockwise, 0 not decided yet
  bool holding_ = false;    // hold: currently mid-dwell at center
  uint32_t lastEdgeMs_ = 0; // hold: dwell start; shake: time of the last counted edge

  bool pressing_ = false;      // press: click currently down
  bool pressFired_ = false;    // press: this press already counted, waiting on release
  uint32_t pressStartMs_ = 0;  // press: when the click went down

  bool wasUp_ = false;        // flick: direction was Up last update
  uint32_t upEdgeMs_ = 0;     // flick: when Up last started
  bool clickHeld_ = false;    // flick: click was down last update
  uint32_t clickEdgeMs_ = 0;  // flick: when the click last went down
  bool flickFired_ = false;   // flick: this click+up already counted, waiting on release
};

} // namespace pan
