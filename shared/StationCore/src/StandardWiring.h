#pragma once
// The wiring every station shares: an RC522 reader and a WS2812B LED strip.
// Station-specific inputs (limit switch, joystick) are wired in each station's
// own project.
//
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
// input-only (fine for analog inputs, see the pan), GPIO12 is a boot strapping
// pin, and GPIO9/10/11 (D2/D3/CMD) are wired to the module's flash.
//
// WS2812B strip (5 pixels), also on the left header:
//   Strip DIN -> GPIO13   (via ~330 ohm series resistor, next to the GND pin)
//   Strip 5V  -> 5V       (bottom pin of left header, USB 5V)
//   Strip GND -> GND      (must share ground with the ESP32)
// Despite the "SPI" in the product name, WS2812B is a single-wire protocol:
// only DIN is needed (DOUT is for chaining more pixels).

#include <stdint.h>

namespace wiring {

constexpr uint8_t kRfidSs = 32;
constexpr uint8_t kRfidSck = 33;
constexpr uint8_t kRfidMosi = 25;
constexpr uint8_t kRfidMiso = 26;
constexpr uint8_t kRfidRst = 27;

constexpr uint8_t kLedPin = 13;
constexpr uint8_t kLedCount = 5;
constexpr uint8_t kLedBrightness = 80; // 0-255

} // namespace wiring
