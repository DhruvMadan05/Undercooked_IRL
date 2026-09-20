#pragma once
// Turns joystick positions into "pattern steps". Pure logic (no Arduino, no
// hardware) so it can be unit tested on the host.
//
// The stick is reduced to one of four directions (or none, in the dead zone).
// Feed every reading to update(); it returns true each time the player makes
// the next move of the pattern:
//   Circle  sweep round the edge. The first direction counts, the second one
//           picks clockwise or counter-clockwise, after which each step has to
//           be the next direction in that same rotation. Wobbling back and
//           forth, or jumping across the stick, does not count.
//   Zigzag  swing left, right, left, ... only Left and Right count and each one
//           has to be the opposite of the last.
// The numbers match oc::Pattern in OvercookedComm.h (JoystickPatternTask.h
// checks that at compile time).

#include <math.h>
#include <stdint.h>

namespace pan {

constexpr uint8_t kCircle = 0;
constexpr uint8_t kZigzag = 1;

enum class Direction : int8_t { None = -1, Up = 0, Right = 1, Down = 2, Left = 3 };

// x: right is positive, y: up is positive, both -1..1. Inside the dead zone
// (radius deadZone) there is no direction.
inline Direction quantize(float x, float y, float deadZone) {
  if (x * x + y * y < deadZone * deadZone) return Direction::None;
  if (fabsf(y) > fabsf(x)) return y > 0 ? Direction::Up : Direction::Down;
  return x > 0 ? Direction::Right : Direction::Left;
}

class PatternTracker {
 public:
  // Forget where the player was, and switch to a pattern.
  void reset(uint8_t pattern) {
    pattern_ = pattern;
    last_ = Direction::None;
    spin_ = 0;
  }

  // True when this reading completes one more step of the pattern.
  bool update(Direction d) {
    if (d == Direction::None || d == last_) return false;

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

  uint8_t pattern_ = kCircle;
  Direction last_ = Direction::None;
  int8_t spin_ = 0; // circle: +1 clockwise, -1 counter-clockwise, 0 not decided yet
};

} // namespace pan
