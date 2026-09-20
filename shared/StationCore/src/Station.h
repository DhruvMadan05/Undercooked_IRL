#pragma once
// Glue for one station: reader events -> messages to the server, server
// answers -> task + LEDs. A station's main.cpp only builds the hardware
// (reader, strip, task) and calls begin() / update().
//
// Per tag on the reader:
//   placed   -> TagPlaced sent, wait for the server
//   Accept   -> task starts (or resumes) with the server's goal and progress
//   Reject   -> red flash, task stays off
//   goal hit -> TaskDone sent, green flash
//   removed  -> TagRemoved sent with the task's progress, task stops
// The server owns all game state (which tag is what, how far along), so a tag
// can be picked up and put back, even on another station of the same kind.

#include <OvercookedComm.h>
#include <PresenceReader.h>

#include "Display.h"
#include "Session.h"
#include "StationTask.h"

namespace station {

class Station {
 public:
  // task may be null for a station with no input task (pot, plate, delivery).
  Station(oc::StationKind kind, tagreader::PresenceReader &reader, Display &display,
          StationTask *task = nullptr)
      : reader_(reader), display_(display), task_(task), session_(kind) {}

  // Call after oc::begin().
  void begin();
  void update(uint32_t now);

 private:
  enum class State : uint8_t {
    Empty,     // no tag on the reader
    Awaiting,  // TagPlaced sent, no answer yet
    Active,    // task running
    Passive,   // accepted, nothing to measure here (server drives the display)
    Rejected,  // server said no, wait for the tag to leave
    Finished,  // task done, wait for the tag to leave
  };

  static constexpr uint32_t kProgressIntervalMs = 250;

  void onPlaced(const tagreader::Uid &uid);
  void onRemoved(const tagreader::Uid &uid);
  void announceTag();
  void reportProgress(uint32_t now);
  oc::TagId tagOf(const tagreader::Uid &uid) const;

  tagreader::PresenceReader &reader_;
  Display &display_;
  StationTask *task_;
  Session session_;

  State state_ = State::Empty;
  oc::TagId tag_ = {};
  uint16_t goal_ = 1;
  uint16_t lastReported_ = 0;
  uint32_t nextProgressAt_ = 0;
};

} // namespace station
