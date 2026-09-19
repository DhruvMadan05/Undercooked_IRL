#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <esp_now.h>

// Standard RC522 breakout (SDA/SCK/MOSI/MISO/IRQ/GND/RST/3.3V) only
// supports SPI, not I2C. On the ESP32-WROVER-IE dev board every RC522
// signal is wired to the LEFT-hand header, using the GPIO matrix to
// remap SPI onto pins 32/33/25/26 (top to bottom on the header):
//   RC522 SDA  -> GPIO32  (SS/CS)
//   RC522 SCK  -> GPIO33
//   RC522 MOSI -> GPIO25
//   RC522 MISO -> GPIO26
//   RC522 RST  -> GPIO27
//   RC522 GND  -> GND     (left header, between GPIO12 and GPIO13)
//   RC522 3.3V -> 3V3     (top of left header; NOT 5V, the chip is not 5V tolerant)
//   RC522 IRQ  -> not connected
//
// Deliberately skipped on the left header: GPIO34/35/36(VP)/39(VN) are
// input-only, GPIO12 is a boot strapping pin, and GPIO9/10/11 (D2/D3/CMD)
// are wired to the module's flash.
#define SS_PIN   32
#define SCK_PIN  33
#define MOSI_PIN 25
#define MISO_PIN 26
#define RST_PIN  27

MFRC522 rfid(SS_PIN, RST_PIN);

// This exact firmware runs on both boards. Each one reads its own RC522
// and broadcasts the UID over ESP-NOW; broadcasting (instead of sending
// to one hardcoded MAC) means neither board needs to know the other's
// address ahead of time.
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
  uint8_t size;
  uint8_t uid[10]; // MFRC522 UIDs are at most 10 bytes
} TagMessage;

TagMessage outgoing;

void printUidHex(const uint8_t *uid, uint8_t size) {
  for (uint8_t i = 0; i < size; i++) {
    Serial.printf(" %02X", uid[i]);
  }
}

void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != sizeof(TagMessage)) return;
  const TagMessage *msg = (const TagMessage *)data;

  Serial.printf("Received tag from %02X:%02X:%02X:%02X:%02X:%02X - UID:",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  printUidHex(msg->uid, msg->size);
  Serial.println();
}

void setup() {
  Serial.begin(115200);

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, -1); // CS is driven by MFRC522 via SS_PIN
  rfid.PCD_Init();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }

  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
  }

  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("RC522 ready. Scan a card...");
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    Serial.println("No tag detected");
    delay(100);
    return;
  }

  Serial.print("UID:");
  printUidHex(rfid.uid.uidByte, rfid.uid.size);
  Serial.println();

  outgoing.size = rfid.uid.size;
  memcpy(outgoing.uid, rfid.uid.uidByte, rfid.uid.size);
  esp_now_send(broadcastAddress, (uint8_t *)&outgoing, sizeof(outgoing));

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
