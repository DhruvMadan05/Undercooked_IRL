#include "Session.h"

namespace station {

void Session::begin() {
  oc::on<oc::MsgType::Welcome>([this](const uint8_t *mac, const oc::WelcomeMsg &msg) {
    if (msg.protocolVersion != oc::kProtocolVersion) {
      Serial.printf("Server speaks protocol %u, this station %u\n", msg.protocolVersion,
                    oc::kProtocolVersion);
      return;
    }
    memcpy(serverMac_, mac, 6);
    lastHeardAt_ = millis();
    setConnected(true);
    if (onWelcome_) onWelcome_();
  });
}

void Session::heardServer(const uint8_t *mac) {
  if (connected_ && memcmp(mac, serverMac_, 6) == 0) lastHeardAt_ = millis();
}

void Session::setConnected(bool connected) {
  if (connected_ == connected) return;
  connected_ = connected;
  Serial.println(connected ? "Server connected" : "Server lost");
  if (onChange_) onChange_(connected);
}

void Session::update(uint32_t now) {
  if (connected_ && (now - lastHeardAt_) >= kTimeoutMs) setConnected(false);

  if ((int32_t)(now - nextSendAt_) < 0) return;
  nextSendAt_ = now + kIntervalMs;

  if (connected_) {
    send<oc::MsgType::Heartbeat>({kind_});
  } else {
    oc::send<oc::MsgType::Hello>({kind_, kFirmwareVersion});
  }
}

} // namespace station
