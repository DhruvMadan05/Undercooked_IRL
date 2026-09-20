#include <Arduino.h>
#include <MFRC522.h>
#include <OvercookedComm.h>
#include <PresenceReader.h>
#include <SPI.h>
#include <WiFi.h>

// Bridge between the ESP-NOW stations and the game server on the laptop.
// It has no game logic: it relays station messages to the laptop over USB
// serial, relays the laptop's messages to stations, and reports tags read by
// its own RC522 (used to calibrate the game: enrolling foods and plates).
//
// Serial IS the data channel, so never Serial.print debug text; use logLine().
//
// Line protocol (ASCII, one message per line, payloads in hex):
//   bridge -> laptop
//     BRIDGE <mac12> <protocol>        reply to INFO, and sent once at boot
//     RX <mac12> <type> <payload>      a station message (type = oc::MsgType)
//     TXFAIL <mac12> <type>            a reliable message never got its ack
//     TAG <uid>                        a tag was put on the bridge's own reader
//     PONG                             reply to PING
//     LOG <text>
//   laptop -> bridge
//     TX <mac12|*> <type> <R|U> <payload>   R = reliable (acked, resent), U = fire and forget
//     PING
//     INFO
// mac12 is 12 hex digits without separators; "*" broadcasts.
//
// Same RC522 wiring as the stations (see overcooked_cutting_board/src/main.cpp):
//   RC522 SDA -> GPIO32, SCK -> GPIO33, MOSI -> GPIO25, MISO -> GPIO26,
//   RST -> GPIO27, GND -> GND, 3.3V -> 3V3 (not 5V).
#define SS_PIN   32
#define SCK_PIN  33
#define MOSI_PIN 25
#define MISO_PIN 26
#define RST_PIN  27

constexpr uint32_t kSameTagDebounceMs = 1000; // ignore the same tag re-read this soon
constexpr size_t kLineMax = 160;

MFRC522 rfid(SS_PIN, RST_PIN);
tagreader::PresenceReader reader(rfid);

void logLine(const char *text) {
  Serial.print("LOG ");
  Serial.println(text);
}

void printHex(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) Serial.printf("%02X", data[i]);
}

void printMac(const uint8_t *mac) {
  printHex(mac, 6);
}

void printInfo() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  Serial.print("BRIDGE ");
  printMac(mac);
  Serial.printf(" %u\n", oc::kProtocolVersion);
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes pairs of hex digits into out. Returns the byte count, or -1 if the
// text is not valid hex or does not fit.
int parseHex(const char *text, uint8_t *out, size_t maxLen) {
  size_t n = strlen(text);
  if (n % 2 != 0 || n / 2 > maxLen) return -1;
  for (size_t i = 0; i < n / 2; i++) {
    int hi = hexValue(text[2 * i]);
    int lo = hexValue(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (hi << 4) | lo;
  }
  return n / 2;
}

// TX <mac12|*> <type> <R|U> [payload]
void handleTx(char *args) {
  char *mac12 = strtok(args, " ");
  char *typeText = strtok(nullptr, " ");
  char *mode = strtok(nullptr, " ");
  char *payloadText = strtok(nullptr, " ");
  if (!mac12 || !typeText || !mode || (mode[0] != 'R' && mode[0] != 'U')) {
    logLine("bad TX line");
    return;
  }

  uint8_t mac[6];
  bool broadcast = strcmp(mac12, "*") == 0;
  if (!broadcast && parseHex(mac12, mac, 6) != 6) {
    logLine("bad TX mac");
    return;
  }

  uint8_t payload[oc::kMaxPayload];
  int len = payloadText ? parseHex(payloadText, payload, sizeof(payload)) : 0;
  if (len < 0) {
    logLine("bad TX payload");
    return;
  }

  uint8_t type = atoi(typeText);
  if (!oc::raw::send(broadcast ? nullptr : mac, type, mode[0] == 'R', payload, len)) {
    Serial.print("TXFAIL ");
    if (broadcast) {
      Serial.print("FFFFFFFFFFFF");
    } else {
      printMac(mac);
    }
    Serial.printf(" %u\n", type);
  }
}

void handleLine(char *line) {
  if (strcmp(line, "PING") == 0) {
    Serial.println("PONG");
  } else if (strcmp(line, "INFO") == 0) {
    printInfo();
  } else if (strncmp(line, "TX ", 3) == 0) {
    handleTx(line + 3);
  } else if (line[0] != '\0') {
    logLine("unknown command");
  }
}

// Collects serial bytes into lines without blocking.
void readSerial() {
  static char line[kLineMax];
  static size_t len = 0;
  static bool overflow = false;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (overflow) {
        logLine("line too long");
      } else {
        line[len] = '\0';
        handleLine(line);
      }
      len = 0;
      overflow = false;
    } else if (len < kLineMax - 1) {
      line[len++] = c;
    } else {
      overflow = true;
    }
  }
}

// Forward every station -> bridge message untouched. Registering a handler is
// also what makes the bridge ack the type.
void relayStationMessages() {
  const oc::MsgType types[] = {
      oc::MsgType::Hello,     oc::MsgType::Heartbeat, oc::MsgType::TagPlaced,
      oc::MsgType::TagRemoved, oc::MsgType::TaskProgress, oc::MsgType::TaskDone,
  };
  for (oc::MsgType type : types) {
    uint8_t typeId = (uint8_t)type;
    oc::raw::setHandler(typeId, [typeId](const uint8_t *mac, const uint8_t *payload, size_t len) {
      Serial.print("RX ");
      printMac(mac);
      Serial.printf(" %u ", typeId);
      printHex(payload, len);
      Serial.println();
    });
  }
}

void reportOwnTag(const tagreader::Uid &uid) {
  static tagreader::Uid last;
  static uint32_t lastAt = 0;
  uint32_t now = millis();
  if (last == uid && (now - lastAt) < kSameTagDebounceMs) return;
  last = uid;
  lastAt = now;

  Serial.print("TAG ");
  printHex(uid.bytes, uid.size);
  Serial.println();
}

void setup() {
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, -1); // CS is driven by MFRC522 via SS_PIN
  bool readerOk = reader.begin();

  if (!oc::begin()) {
    while (true) {
      logLine("ESP-NOW init failed");
      delay(1000);
    }
  }
  relayStationMessages();
  oc::onSendFailed([](const uint8_t *mac, oc::MsgType type) {
    Serial.print("TXFAIL ");
    printMac(mac);
    Serial.printf(" %u\n", (uint8_t)type);
  });

  printInfo();
  if (!readerOk) logLine("RC522 not answering, check the wiring");
}

void loop() {
  oc::poll();
  readSerial();

  tagreader::Change change = reader.poll(millis());
  if (change.placed) reportOwnTag(change.placedUid);
}
