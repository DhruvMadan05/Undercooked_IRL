#pragma once
// What a station tells its display(s): the same calls Station already made on
// Display (the LED strip), pulled out so a station can also drive a second,
// independent display (e.g. the pan's OLED) off the exact same server
// messages, with no protocol change - Station just forwards every call to
// both instead of one.

#include <OvercookedComm.h>

namespace station {

class DisplaySink {
 public:
  virtual ~DisplaySink() = default;

  virtual void begin() {}
  virtual void setOffline(bool offline) {}

  // From the server. Calibrated / Success / Reject are one-shots (flash());
  // the rest (setMode()) stay until the next call.
  virtual void setMode(oc::DisplayMode mode, uint8_t level) = 0;
  virtual void flash(oc::DisplayMode mode) {}

  // Task progress: value out of goal. clearProgress() hides it.
  virtual void setProgress(uint16_t value, uint16_t goal) {}
  virtual void clearProgress() {}

  virtual void update(uint32_t now) = 0;
};

} // namespace station
