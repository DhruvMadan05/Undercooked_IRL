#pragma once
// The station-specific job: what the hardware does while a tag is on it.
// Cutting board = count button presses, pan = follow a joystick pattern, ...
// The server decides which tag gets which task and how big the goal is; the
// task only measures the player's input.

#include <OvercookedComm.h>

namespace station {

class StationTask {
 public:
  virtual ~StationTask() = default;

  // Which task this hardware can run. The server's Accept only starts it when
  // it matches.
  virtual oc::TaskKind kind() const = 0;

  virtual void begin() {}

  // A tag was accepted: count towards goal, starting from progress (resume).
  virtual void start(uint16_t goal, uint16_t progress, uint8_t param) = 0;

  // The tag left (or was rejected): ignore input until the next start().
  virtual void stop() = 0;

  // Every loop. Sample inputs here, never block.
  virtual void update(uint32_t now) = 0;

  virtual uint16_t progress() const = 0; // task units done so far
  virtual bool done() const = 0;         // progress reached the goal while running
};

} // namespace station
