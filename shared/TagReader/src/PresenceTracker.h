#pragma once
// Turns "which tag did the reader see on this poll" into placed / removed
// events. Pure logic (no Arduino, no hardware) so it can be unit tested on the
// host.
//
// A tag counts as removed only after missLimit polls in a row saw nothing,
// because an RC522 misses a tag now and then even when it sits on the reader.

#include <stdint.h>
#include <string.h>

namespace tagreader {

constexpr uint8_t kMaxUid = 10;

struct Uid {
  uint8_t size = 0;
  uint8_t bytes[kMaxUid] = {};

  bool operator==(const Uid &o) const {
    return size == o.size && memcmp(bytes, o.bytes, size) == 0;
  }
};

// What one poll changed. A different tag replacing the current one reports
// both: removed (the old uid) then placed (the new one).
struct Change {
  bool removed = false;
  bool placed = false;
  Uid removedUid;
  Uid placedUid;
};

class PresenceTracker {
 public:
  explicit PresenceTracker(uint8_t missLimit = 3) : missLimit_(missLimit) {}

  // uid == nullptr means the reader saw no tag on this poll.
  Change update(const uint8_t *uid, uint8_t size) {
    Change change;
    if (uid == nullptr || size == 0) {
      if (present_ && ++misses_ >= missLimit_) {
        change.removed = true;
        change.removedUid = current_;
        present_ = false;
        misses_ = 0;
      }
      return change;
    }

    Uid seen;
    seen.size = size > kMaxUid ? kMaxUid : size;
    memcpy(seen.bytes, uid, seen.size);

    if (present_ && current_ == seen) {
      misses_ = 0;
      return change;
    }
    if (present_) {
      change.removed = true;
      change.removedUid = current_;
    }
    change.placed = true;
    change.placedUid = seen;
    current_ = seen;
    present_ = true;
    misses_ = 0;
    return change;
  }

  bool present() const { return present_; }
  const Uid &current() const { return current_; }

 private:
  uint8_t missLimit_;
  uint8_t misses_ = 0;
  bool present_ = false;
  Uid current_;
};

} // namespace tagreader
