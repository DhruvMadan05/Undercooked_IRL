#pragma once
// Deep fryer scoring: how well the player's hand height tracks a roaming
// target. Pure logic (no Arduino, no hardware) so it can be unit tested on
// the host; FryTask.h supplies the hand and target positions (from the
// ultrasonic sensor and the roaming motion) and feeds their overlap in here.
//
// overlaps() says whether the hand is inside the target's catch zone.
// ProgressTracker turns a stream of overlap/dt readings into progress:
// filling while inside the zone, draining while outside, clamped to 0..1.
// A caller-controlled pause (e.g. the post-scan grace period while the
// ultrasonic settles) just skips the integration for that tick.

namespace fryer {

constexpr float kStartProgress = 0.05f; // small head start: a miss on the very first tick should not be an instant fail
constexpr float kFillPerSec = 1.0f / 10.0f;  // 10s of solid overlap: empty -> full
constexpr float kDrainPerSec = 1.0f / 60.0f; // 60s of solid miss: full -> empty

// handPos/targetPos are 0..1 positions on the same axis; catchZoneFrac is how
// much of that range counts as a hit, centered on the target.
inline bool overlaps(float handPos, float targetPos, float catchZoneFrac) {
  float d = handPos - targetPos;
  if (d < 0) d = -d;
  return d <= catchZoneFrac / 2.0f;
}

class ProgressTracker {
 public:
  // startAt: resume from a saved fraction (progress/goal), clamped to 0..1.
  void reset(float startAt = kStartProgress) {
    if (startAt < 0.0f) startAt = 0.0f;
    if (startAt > 1.0f) startAt = 1.0f;
    progress_ = startAt;
  }

  // dt in seconds. paused skips the integration (progress unchanged), e.g.
  // during the settling grace period right after a tag is scanned.
  void update(bool overlap, float dt, bool paused) {
    if (paused) return;
    progress_ += (overlap ? kFillPerSec : -kDrainPerSec) * dt;
    if (progress_ < 0.0f) progress_ = 0.0f;
    if (progress_ > 1.0f) progress_ = 1.0f;
  }

  float progress() const { return progress_; }
  bool done() const { return progress_ >= 1.0f; }

 private:
  float progress_ = kStartProgress;
};

} // namespace fryer
