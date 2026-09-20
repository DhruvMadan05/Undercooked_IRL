#pragma once

// Shared hardware pin map for the frying pan Simon Says build (FryingPanSimonSays.cpp
// and JoystickTest.cpp), so the two can't drift out of sync with the physical wiring.
//
// Joystick + OLED live on the LEFT-hand header of the ESP32-DevKitC. These are
// the same GPIO32/33 (and RfidTagReader.cpp's GPIO25/26/27) used elsewhere on
// this header - safe to double up since main.cpp only ever runs one of the
// joystick/Simon-Says build or the RFID build at a time, never both.
// Deliberately skipped on this header: GPIO34/35 (input-only, unused here),
// GPIO12 (boot strapping pin), and GPIO9/10/11 (wired to the internal SPI flash).

#define JOYSTICK_X_PIN     39  // ADC0, input-only - fine for an analog axis
#define JOYSTICK_Y_PIN     36  // ADC3, input-only - fine for an analog axis
#define JOYSTICK_CLICK_PIN 14
#define JOYSTICK_DEADZONE  75  // out of the Better Joystick library's 0-1023 range

// Two OLEDs, both 128x32 SSD1306. They run on separate I2C buses rather than
// sharing one, because both modules ship hardwired to address 0x3C.
// MOVE screen (0.96"): the move animation only.
// BAR screen (0.91"):  the cook/burn timer bar only.
#define OLED_MOVE_SDA_PIN  32  // left header, same side as the joystick (Wire)
#define OLED_MOVE_SCL_PIN  33
#define OLED_BAR_SDA_PIN   25  // second I2C peripheral (Wire1)
#define OLED_BAR_SCL_PIN   26
#define OLED_ADDRESS       0x3C
#define SCREEN_WIDTH       128
#define SCREEN_HEIGHT      32
