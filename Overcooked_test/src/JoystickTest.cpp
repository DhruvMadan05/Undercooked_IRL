#include "JoystickTest.h"

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BetterJoystick.h>

#include "FryingPanPins.h"

namespace {

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Joystick stick(JOYSTICK_X_PIN, JOYSTICK_Y_PIN, JOYSTICK_CLICK_PIN);

const char *directionLabel(int dx, int dy) {
  if (abs(dx) < JOYSTICK_DEADZONE && abs(dy) < JOYSTICK_DEADZONE) return "CENTER";
  if (abs(dx) > abs(dy)) return dx > 0 ? "RIGHT" : "LEFT";
  return dy > 0 ? "UP" : "DOWN"; // flip if your module reports the opposite sign for up/down
}

} // namespace

void joystickTestSetup() {
  Serial.begin(115200);

  analogReadResolution(10); // matches Better Joystick's hardcoded 0-1023 assumption

  Wire.begin(OLED_MOVE_SDA_PIN, OLED_MOVE_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 init failed");
    while (true) delay(1000);
  }
  display.clearDisplay();
  display.display();

  Serial.println("Joystick test ready. Move the stick / click it.");
}

void joystickTestLoop() {
  int x = stick.x();
  int y = stick.y();
  bool clicked = stick.isPressed();
  int dx = x - 512;
  int dy = y - 512;
  const char *dir = directionLabel(dx, dy);

  Serial.printf("X=%4d Y=%4d dx=%5d dy=%5d click=%d dir=%s\n", x, y, dx, dy, clicked, dir);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("X:");
  display.print(x);
  display.print("  Y:");
  display.print(y);

  display.setCursor(0, 10);
  display.print("Click: ");
  display.print(clicked ? "YES" : "no");

  display.setTextSize(2);
  display.setCursor(0, 20);
  display.print(dir);

  display.display();

  delay(100);
}
