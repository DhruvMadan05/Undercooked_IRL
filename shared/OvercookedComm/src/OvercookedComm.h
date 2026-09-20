#pragma once
// ESP-NOW messaging between the stations and the bridge ESP (which relays to
// the game server on the laptop over USB serial).
//
// Flow:
//   station --Hello-->        bridge   station boots, declares its kind (until Welcome)
//   bridge  --Welcome-->      station  the station learns the bridge's MAC
//   station --Heartbeat-->    bridge   every second, so the server knows it is alive
//   station --TagPlaced-->    bridge   a tag arrived on the reader
//   bridge  --Accept-->       station  start (or resume) this task on that tag
//   bridge  --Reject-->       station  the tag does not belong here
//   station --TaskProgress--> bridge   task progress, latest value wins
//   station --TaskDone-->     bridge   task finished
//   station --TagRemoved-->   bridge   the tag left the reader
//   bridge  --SetDisplay-->   station  what the LEDs should show
//
// Usage (both sides):
//   oc::begin();                                         // in setup()
//   oc::on<oc::MsgType::Accept>(handler);                // handler(mac, msg)
//   oc::poll();                                          // in loop()
//   oc::send<oc::MsgType::TagPlaced>({tag});             // broadcast
//   oc::sendTo<oc::MsgType::Accept>(mac, {...});         // to one device
//
// Handlers run inside oc::poll(), i.e. on the main loop, not in the WiFi
// task, so they can touch your normal program state without locking.
//
// Delivery: a message is either reliable or not, decided per type by
// Payload<M>::reliable.
//   Reliable: send / sendTo only queue the message; oc::poll() transmits it
//   and resends every kRetryIntervalMs until the receiver acks, up to
//   kMaxAttempts sends, then reports it through oc::onSendFailed(). Each
//   destination has its own queue and sends one message at a time in order, so
//   a dead station only delays messages to itself. A device acks a message
//   only if it has a handler registered for that type (so on a broadcast just
//   the intended receiver answers), and it drops resends of a message it
//   already handled, so a lost ack never makes a handler run twice.
//   Unreliable: transmitted immediately, never acked or resent. Use it for
//   state that is refreshed anyway (progress, display, heartbeat).
//
// To add a new message:
//   1. add a value to MsgType, before Count (append only: values are wire IDs)
//   2. define its payload struct (packed plain data, at most kMaxPayload bytes)
//   3. map them together with a Payload<> specialization below
//   4. mirror it in overcooked_game/overcooked/protocol.py
// send / sendTo / on then accept only that payload type for that message.

#include <Arduino.h>
#include <functional>
#include <type_traits>

namespace oc {

constexpr uint8_t kProtocolVersion = 2;
constexpr uint8_t kMaxUidSize = 10; // MFRC522 UIDs are at most 10 bytes
constexpr size_t kMaxPayload = 32;

// Delivery tuning for reliable messages (see "Delivery" above).
constexpr uint8_t kMaxAttempts = 5;         // total sends per message
constexpr uint32_t kRetryIntervalMs = 200;  // wait for an ack before resending

#define OC_PACKED __attribute__((packed))

struct OC_PACKED TagId {
  uint8_t size;
  uint8_t bytes[kMaxUidSize];
};

// What kind of station a device is. Compiled into each station's firmware.
enum class StationKind : uint8_t {
  CuttingBoard = 0,
  Pan = 1,
  Pot = 2,
  Plate = 3,
  Delivery = 4,
};

// What the station's own hardware has to do while a tag is on it.
enum class TaskKind : uint8_t {
  None = 0,            // presence only, the server drives everything
  Presses = 1,         // count button presses
  JoystickPattern = 2, // move the joystick in a pattern
};

// Accept.param for TaskKind::JoystickPattern: the stick movement to perform.
enum class Pattern : uint8_t {
  Circle = 0, // sweep the stick round the edge, either way
  Zigzag = 1, // swing it left and right
};

// What the LEDs show when no task is drawing its own progress.
enum class DisplayMode : uint8_t {
  Idle = 0,
  Calibrated = 1,
  Cooking = 2, // level = progress 0-255
  Warning = 3, // about to burn
  Burnt = 4,
  Success = 5,
  Reject = 6,
  Disconnected = 7,
  GameOver = 8,
};

// ---- Messages --------------------------------------------------------------

enum class MsgType : uint8_t {
  Hello = 0,        // station -> bridge
  Heartbeat = 1,    // station -> bridge
  TagPlaced = 2,    // station -> bridge
  TagRemoved = 3,   // station -> bridge
  TaskProgress = 4, // station -> bridge
  TaskDone = 5,     // station -> bridge
  Welcome = 6,      // bridge -> station
  Accept = 7,       // bridge -> station
  Reject = 8,       // bridge -> station
  SetDisplay = 9,   // bridge -> station
  Count             // keep last
};

struct OC_PACKED HelloMsg {
  StationKind kind;
  uint8_t fwVersion;
};

struct OC_PACKED HeartbeatMsg {
  StationKind kind;
};

struct OC_PACKED TagPlacedMsg {
  TagId tag;
};

struct OC_PACKED TagRemovedMsg {
  TagId tag;
  uint16_t progress; // task progress when it left, so the server keeps it exactly
};

struct OC_PACKED TaskProgressMsg {
  TagId tag;
  uint16_t value; // in task units (presses, pattern steps), out of the goal
};

struct OC_PACKED TaskDoneMsg {
  TagId tag;
};

struct OC_PACKED WelcomeMsg {
  uint8_t protocolVersion;
};

struct OC_PACKED AcceptMsg {
  TagId tag;
  TaskKind task;
  uint16_t goal;     // task units to finish
  uint16_t progress; // task units already done (resume)
  uint8_t param;     // task specific, e.g. joystick pattern id
};

struct OC_PACKED RejectMsg {
  TagId tag;
};

struct OC_PACKED SetDisplayMsg {
  DisplayMode mode;
  uint8_t level;
};

static_assert(sizeof(TagId) == 11, "wire layout changed: update protocol.py");
static_assert(sizeof(HelloMsg) == 2, "wire layout changed: update protocol.py");
static_assert(sizeof(HeartbeatMsg) == 1, "wire layout changed: update protocol.py");
static_assert(sizeof(TagPlacedMsg) == 11, "wire layout changed: update protocol.py");
static_assert(sizeof(TagRemovedMsg) == 13, "wire layout changed: update protocol.py");
static_assert(sizeof(TaskProgressMsg) == 13, "wire layout changed: update protocol.py");
static_assert(sizeof(TaskDoneMsg) == 11, "wire layout changed: update protocol.py");
static_assert(sizeof(WelcomeMsg) == 1, "wire layout changed: update protocol.py");
static_assert(sizeof(AcceptMsg) == 17, "wire layout changed: update protocol.py");
static_assert(sizeof(RejectMsg) == 11, "wire layout changed: update protocol.py");
static_assert(sizeof(SetDisplayMsg) == 2, "wire layout changed: update protocol.py");

template <MsgType M> struct Payload;
template <> struct Payload<MsgType::Hello>        { using type = HelloMsg;        static constexpr bool reliable = true; };
template <> struct Payload<MsgType::Heartbeat>    { using type = HeartbeatMsg;    static constexpr bool reliable = false; };
template <> struct Payload<MsgType::TagPlaced>    { using type = TagPlacedMsg;    static constexpr bool reliable = true; };
template <> struct Payload<MsgType::TagRemoved>   { using type = TagRemovedMsg;   static constexpr bool reliable = true; };
template <> struct Payload<MsgType::TaskProgress> { using type = TaskProgressMsg; static constexpr bool reliable = false; };
template <> struct Payload<MsgType::TaskDone>     { using type = TaskDoneMsg;     static constexpr bool reliable = true; };
template <> struct Payload<MsgType::Welcome>      { using type = WelcomeMsg;      static constexpr bool reliable = true; };
template <> struct Payload<MsgType::Accept>       { using type = AcceptMsg;       static constexpr bool reliable = true; };
template <> struct Payload<MsgType::Reject>       { using type = RejectMsg;       static constexpr bool reliable = true; };
template <> struct Payload<MsgType::SetDisplay>   { using type = SetDisplayMsg;   static constexpr bool reliable = false; };

template <MsgType M> using PayloadOf = typename Payload<M>::type;

// ---- Setup and dispatch ----------------------------------------------------

// Starts WiFi in station mode and ESP-NOW. Returns false on failure.
bool begin();

// Call every loop(): runs the handlers for messages received since the last
// call, and sends / resends queued reliable messages.
void poll();

// Called from poll() when a reliable message still had no ack after
// kMaxAttempts sends. It is dropped and the next one for that device goes out.
// mac is the destination (all 0xFF for a broadcast).
void onSendFailed(std::function<void(const uint8_t *mac, MsgType type)> handler);

// ---- Helpers ---------------------------------------------------------------

TagId makeTagId(const uint8_t *uid, uint8_t size);
bool sameTag(const TagId &a, const TagId &b);
void printTag(const TagId &tag); // "AB CD EF", no newline
bool isBroadcast(const uint8_t *mac);

// ---- Sending and receiving -------------------------------------------------

// Untyped access for the bridge, which relays messages it does not interpret.
// Types >= MsgType::Count are refused.
namespace raw {
using Handler = std::function<void(const uint8_t *mac, const uint8_t *payload, size_t len)>;

// mac == nullptr broadcasts. Returns false if it could not be queued / sent.
bool send(const uint8_t *mac, uint8_t type, bool reliable, const void *payload, size_t len);

// Registers a handler for one type. Having a handler is what makes this
// device ack that type.
void setHandler(uint8_t type, Handler handler);
} // namespace raw

// Broadcast to every device in range. Returns false if the message could not
// be queued (outbox full); true does not mean delivered.
template <MsgType M> bool send(const PayloadOf<M> &msg) {
  static_assert(std::is_trivially_copyable<PayloadOf<M>>::value, "payload must be plain data");
  static_assert(sizeof(PayloadOf<M>) <= kMaxPayload, "payload too large");
  return raw::send(nullptr, (uint8_t)M, Payload<M>::reliable, &msg, sizeof(msg));
}

// Send to one device, e.g. the mac a message arrived from.
template <MsgType M> bool sendTo(const uint8_t *mac, const PayloadOf<M> &msg) {
  static_assert(std::is_trivially_copyable<PayloadOf<M>>::value, "payload must be plain data");
  static_assert(sizeof(PayloadOf<M>) <= kMaxPayload, "payload too large");
  return raw::send(mac, (uint8_t)M, Payload<M>::reliable, &msg, sizeof(msg));
}

// Register the handler for a message type (one handler per type; a second
// call replaces the first).
template <MsgType M>
void on(std::function<void(const uint8_t *mac, const PayloadOf<M> &msg)> handler) {
  using T = PayloadOf<M>;
  static_assert(std::is_trivially_copyable<T>::value, "payload must be plain data");
  static_assert(sizeof(T) <= kMaxPayload, "payload too large");
  raw::setHandler((uint8_t)M, [handler](const uint8_t *mac, const uint8_t *payload, size_t len) {
    if (len != sizeof(T)) return;
    T msg;
    memcpy(&msg, payload, sizeof(T)); // payload may be unaligned
    handler(mac, msg);
  });
}

} // namespace oc
