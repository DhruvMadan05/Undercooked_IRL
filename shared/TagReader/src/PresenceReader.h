#pragma once
// RC522 wrapper that reports tags being placed on and removed from the reader.
//
// Every poll sends a WUPA (wake-up) instead of the usual REQA. REQA only
// answers tags that are idle, and a tag we halted after reading would go
// silent until it left the field, which would make removal undetectable.
// WUPA also wakes halted tags, so "no answer" really means "no tag".

#include <Arduino.h>
#include <MFRC522.h>

#include "PresenceTracker.h"

namespace tagreader {

class PresenceReader {
 public:
  // pollMs: time between polls. missLimit: polls in a row with no tag before
  // it counts as removed (removal delay is about pollMs * missLimit).
  PresenceReader(MFRC522 &rfid, uint16_t pollMs = 100, uint8_t missLimit = 3)
      : rfid_(rfid), pollMs_(pollMs), tracker_(missLimit) {}

  // Call after SPI.begin(...). Returns false if the RC522 does not answer
  // (bad wiring: the version register reads 0x00 or 0xFF).
  bool begin() {
    rfid_.PCD_Init();
    byte version = rfid_.PCD_ReadRegister(MFRC522::VersionReg);
    return version != 0x00 && version != 0xFF;
  }

  // Call every loop(). Does nothing until the next poll is due.
  Change poll(uint32_t now) {
    if ((int32_t)(now - nextPoll_) < 0) return Change{};
    nextPoll_ = now + pollMs_;

    byte atqa[2];
    byte atqaSize = sizeof(atqa);
    bool seen = rfid_.PICC_WakeupA(atqa, &atqaSize) == MFRC522::STATUS_OK &&
                rfid_.PICC_ReadCardSerial();
    if (!seen) return tracker_.update(nullptr, 0);

    Change change = tracker_.update(rfid_.uid.uidByte, rfid_.uid.size);
    rfid_.PICC_HaltA();
    rfid_.PCD_StopCrypto1();
    return change;
  }

  bool present() const { return tracker_.present(); }
  const Uid &current() const { return tracker_.current(); }

 private:
  MFRC522 &rfid_;
  uint16_t pollMs_;
  PresenceTracker tracker_;
  uint32_t nextPoll_ = 0;
};

} // namespace tagreader
