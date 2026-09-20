#pragma once
// Sink scrubbing: how long a joystick has been *actively* circled. Pure logic
// (no Arduino, no hardware) so it can be unit tested on the host; ScrubTask.h
// reads the stick and feeds its offset from the resting position in here.
//
// The stick counts as scrubbing while its angle around the centre keeps
// changing: at least kMinDeltaAngle between two samples, in either direction.
// Holding it off to one side, or letting it rest near the centre, does not count.

#include <math.h>
#include <stdint.h>

namespace scrub {

constexpr float kDeadzoneRadius = 500.0f; // raw ADC counts off centre before the angle means anything
constexpr float kMinDeltaAngle = 0.05f;   // radians per sample (~3 degrees) to count as scrubbing
constexpr float kPi = 3.14159265f;

class ScrubTracker {
 public:
  // startMs: resume from milliseconds already scrubbed.
  void reset(uint32_t startMs = 0) {
    activeMs_ = startMs;
    haveLast_ = false;
  }

  // dx, dy: the stick's offset from its resting position in ADC counts.
  // dtMs: time since the previous sample.
  void update(float dx, float dy, uint32_t dtMs) {
    if (sqrtf(dx * dx + dy * dy) < kDeadzoneRadius) {
      // Back through the dead zone: forget the angle so the next reading is not one huge jump.
      haveLast_ = false;
      return;
    }
    float angle = atan2f(dy, dx);
    if (haveLast_) {
      float delta = angle - lastAngle_;
      if (delta > kPi) delta -= 2 * kPi;
      if (delta < -kPi) delta += 2 * kPi;
      if (fabsf(delta) >= kMinDeltaAngle) activeMs_ += dtMs;
    }
    lastAngle_ = angle;
    haveLast_ = true;
  }

  uint32_t activeMs() const { return activeMs_; }

 private:
  uint32_t activeMs_ = 0;
  float lastAngle_ = 0;
  bool haveLast_ = false;
};

} // namespace scrub
