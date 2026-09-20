#include "OvercookedComm.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace oc {
namespace {

// Every packet is Header + payload. The magic byte filters out unrelated
// ESP-NOW traffic. An ack is a bare Header with type kAckType and the seq of
// the message it confirms.
constexpr uint8_t kMagic = 0xC7;
constexpr uint8_t kAckType = 0xFF;
constexpr uint8_t kFlagReliable = 0x01;

struct Header {
  uint8_t magic;
  uint8_t type;
  uint8_t seq;
  uint8_t flags;
};

constexpr size_t kMaxPacket = sizeof(Header) + kMaxPayload;
constexpr UBaseType_t kQueueDepth = 16;
constexpr uint8_t kMaxPeers = 12;     // 8 stations + broadcast + spare
constexpr uint8_t kPerPeerDepth = 4;  // reliable messages waiting per destination
constexpr uint8_t kSeenSlots = 12;
const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// A resend of a message is recognised by its sender + seq, but only for this
// long; after that the same seq counts as new (the sender may have rebooted).
// It must outlast every resend of one message.
constexpr uint32_t kDedupeWindowMs = 5000;
static_assert((uint32_t)kMaxAttempts * kRetryIntervalMs < kDedupeWindowMs,
              "dedupe window must cover all retries");

struct Packet {
  uint8_t mac[6];
  uint8_t len;
  uint8_t data[kMaxPacket];
};

QueueHandle_t rxQueue = nullptr;
raw::Handler handlers[(size_t)MsgType::Count];

// Reliable messages wait here, one FIFO per destination. Only the head of
// each is ever on the air, so a resend can never overtake or be overtaken by
// a later message to the same device.
struct Outgoing {
  uint8_t type;
  uint8_t seq;
  uint8_t len;
  uint8_t payload[kMaxPayload];
  uint8_t attempts;        // sends so far
  uint32_t nextAttemptAt;  // resend deadline, valid once attempts > 0
};

struct PeerQueue {
  bool used;
  uint8_t mac[6];
  uint8_t head;
  uint8_t count;
  Outgoing q[kPerPeerDepth];
};

PeerQueue peers[kMaxPeers];
uint8_t nextSeq = 0;
std::function<void(const uint8_t *, MsgType)> sendFailedHandler;

// Last reliable message handled from each sender, to drop resends after a
// lost ack.
struct Seen {
  uint8_t mac[6];
  uint8_t seq;
  uint32_t at;
  bool valid;
};

Seen seen[kSeenSlots];

// Runs in the WiFi task: just copy the packet out and let poll() deal with it.
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < (int)sizeof(Header) || len > (int)kMaxPacket) return;
  Packet p;
  memcpy(p.mac, mac, 6);
  p.len = len;
  memcpy(p.data, data, len);
  xQueueSend(rxQueue, &p, 0); // queue full: drop the packet (a reliable sender resends)
}

bool ensurePeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0; // current WiFi channel
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

bool transmit(const uint8_t *mac, uint8_t type, uint8_t seq, uint8_t flags,
              const void *payload, size_t len) {
  if (!ensurePeer(mac)) return false;
  uint8_t buf[kMaxPacket];
  Header header = {kMagic, type, seq, flags};
  memcpy(buf, &header, sizeof(Header));
  if (len) memcpy(buf + sizeof(Header), payload, len);
  return esp_now_send(mac, buf, sizeof(Header) + len) == ESP_OK;
}

void sendAck(const uint8_t *mac, uint8_t seq) {
  transmit(mac, kAckType, seq, 0, nullptr, 0);
}

// True if this exact message was already handled from this sender. Records
// the message when it is new.
bool seenBefore(const uint8_t *mac, uint8_t seq, uint32_t now) {
  for (Seen &s : seen) {
    if (s.valid && memcmp(s.mac, mac, 6) == 0) {
      if (s.seq == seq && (now - s.at) < kDedupeWindowMs) return true;
      s.seq = seq;
      s.at = now;
      return false;
    }
  }

  // New sender: take a free slot, or evict the one heard from longest ago.
  Seen *victim = &seen[0];
  for (Seen &s : seen) {
    if (!s.valid) { victim = &s; break; }
    if (now - s.at > now - victim->at) victim = &s;
  }
  memcpy(victim->mac, mac, 6);
  victim->seq = seq;
  victim->at = now;
  victim->valid = true;
  return false;
}

PeerQueue *findQueue(const uint8_t *mac, bool create) {
  PeerQueue *free = nullptr;
  for (PeerQueue &pq : peers) {
    if (pq.used && memcmp(pq.mac, mac, 6) == 0) return &pq;
    if (!pq.used && !free) free = &pq;
  }
  if (!create || !free) return nullptr;
  free->used = true;
  memcpy(free->mac, mac, 6);
  free->head = 0;
  free->count = 0;
  return free;
}

void popHead(PeerQueue &pq) {
  pq.head = (pq.head + 1) % kPerPeerDepth;
  if (--pq.count == 0) pq.used = false;
}

// Sends the head of each queue, resends it when its ack is overdue, and gives
// up on it after kMaxAttempts.
void pumpOutbox(uint32_t now) {
  for (uint8_t i = 0; i < kMaxPeers; i++) {
    PeerQueue &pq = peers[i];
    if (!pq.used) continue;
    Outgoing &o = pq.q[pq.head];
    if (o.attempts > 0 && (int32_t)(now - o.nextAttemptAt) < 0) continue;

    if (o.attempts >= kMaxAttempts) {
      MsgType type = (MsgType)o.type;
      uint8_t mac[6];
      memcpy(mac, pq.mac, 6);
      popHead(pq);
      if (sendFailedHandler) sendFailedHandler(mac, type);
      continue;
    }

    transmit(pq.mac, o.type, o.seq, kFlagReliable, o.payload, o.len); // a failure is a lost attempt
    o.attempts++;
    o.nextAttemptAt = now + kRetryIntervalMs;
  }
}

void handleAck(const uint8_t *mac, uint8_t seq) {
  for (PeerQueue &pq : peers) {
    if (!pq.used) continue;
    const Outgoing &o = pq.q[pq.head];
    if (o.attempts == 0 || o.seq != seq) continue;
    // A broadcast is confirmed by whichever device answers.
    if (isBroadcast(pq.mac) || memcmp(pq.mac, mac, 6) == 0) {
      popHead(pq);
      return;
    }
  }
}

void handleMessage(const Packet &p, const Header &header, uint32_t now) {
  if (header.type >= (uint8_t)MsgType::Count) return;

  // No handler = not a message this device consumes. Stay silent so that,
  // for a broadcast, only the intended receiver acks.
  const raw::Handler &handler = handlers[header.type];
  if (!handler) return;

  if (header.flags & kFlagReliable) {
    sendAck(p.mac, header.seq); // ack every copy, the sender may have missed the last ack
    if (seenBefore(p.mac, header.seq, now)) return;
  }
  handler(p.mac, p.data + sizeof(Header), p.len - sizeof(Header));
}

} // namespace

bool begin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) return false;

  rxQueue = xQueueCreate(kQueueDepth, sizeof(Packet));
  if (!rxQueue) return false;

  nextSeq = esp_random() & 0xFF; // so a reboot doesn't reuse the last seq
  esp_now_register_recv_cb(onDataRecv);
  return ensurePeer(kBroadcast);
}

void poll() {
  if (!rxQueue) return;

  Packet p;
  while (xQueueReceive(rxQueue, &p, 0) == pdTRUE) {
    Header header;
    memcpy(&header, p.data, sizeof(Header));
    if (header.magic != kMagic) continue;

    uint32_t now = millis();
    if (header.type == kAckType) {
      handleAck(p.mac, header.seq);
    } else {
      handleMessage(p, header, now);
    }
  }

  pumpOutbox(millis());
}

void onSendFailed(std::function<void(const uint8_t *mac, MsgType type)> handler) {
  sendFailedHandler = std::move(handler);
}

TagId makeTagId(const uint8_t *uid, uint8_t size) {
  TagId tag = {};
  tag.size = size > kMaxUidSize ? kMaxUidSize : size;
  memcpy(tag.bytes, uid, tag.size);
  return tag;
}

bool sameTag(const TagId &a, const TagId &b) {
  return a.size == b.size && memcmp(a.bytes, b.bytes, a.size) == 0;
}

void printTag(const TagId &tag) {
  for (uint8_t i = 0; i < tag.size; i++) {
    Serial.printf(i ? " %02X" : "%02X", tag.bytes[i]);
  }
}

bool isBroadcast(const uint8_t *mac) {
  return memcmp(mac, kBroadcast, 6) == 0;
}

namespace raw {

// Reliable messages are queued (poll() does the sending); the rest go out now.
bool send(const uint8_t *mac, uint8_t type, bool reliable, const void *payload, size_t len) {
  if (type >= (uint8_t)MsgType::Count || len > kMaxPayload) return false;
  const uint8_t *dst = mac ? mac : kBroadcast;
  uint8_t seq = nextSeq++;

  if (!reliable) return transmit(dst, type, seq, 0, payload, len);

  PeerQueue *pq = findQueue(dst, true);
  if (!pq || pq->count >= kPerPeerDepth) return false;

  Outgoing &o = pq->q[(pq->head + pq->count) % kPerPeerDepth];
  o.type = type;
  o.seq = seq;
  o.len = len;
  memcpy(o.payload, payload, len);
  o.attempts = 0;
  o.nextAttemptAt = 0;
  pq->count++;
  return true;
}

void setHandler(uint8_t type, Handler handler) {
  if (type >= (uint8_t)MsgType::Count) return;
  handlers[type] = std::move(handler);
}

} // namespace raw
} // namespace oc
