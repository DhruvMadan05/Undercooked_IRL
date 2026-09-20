#pragma once
// A station's link to the server. Until the server answers it broadcasts
// Hello once a second; after Welcome it learns the bridge's MAC, sends there
// directly and sends a Heartbeat once a second. Any message from the server
// counts as proof of life; three and a half seconds of silence drop the
// station back to broadcasting Hello.

#include <OvercookedComm.h>
#include <functional>

namespace station {

class Session {
 public:
  explicit Session(oc::StationKind kind) : kind_(kind) {}

  // Call after oc::begin().
  void begin();
  void update(uint32_t now);

  // Station calls this for every message it receives from the server.
  void heardServer(const uint8_t *mac);

  bool connected() const { return connected_; }

  // Fires when the link comes up or goes down.
  void onChange(std::function<void(bool connected)> handler) { onChange_ = std::move(handler); }

  // Fires on every Welcome, including one that arrives while already
  // connected: that means the server (re)started and forgot everything we told it.
  void onWelcome(std::function<void()> handler) { onWelcome_ = std::move(handler); }

  // To the server: the learned bridge MAC once welcomed, a broadcast before.
  template <oc::MsgType M> bool send(const oc::PayloadOf<M> &msg) {
    return connected_ ? oc::sendTo<M>(serverMac_, msg) : oc::send<M>(msg);
  }

 private:
  static constexpr uint32_t kIntervalMs = 1000;
  static constexpr uint32_t kTimeoutMs = 3500;
  static constexpr uint8_t kFirmwareVersion = 1;

  void setConnected(bool connected);

  oc::StationKind kind_;
  bool connected_ = false;
  uint8_t serverMac_[6] = {};
  uint32_t lastHeardAt_ = 0;
  uint32_t nextSendAt_ = 0;
  std::function<void(bool)> onChange_;
  std::function<void()> onWelcome_;
};

} // namespace station
