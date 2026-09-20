#pragma once
// Animated joystick-pattern cue on a single 128x64 ("0.96 inch") SSD1306, so
// the player can see which gesture to do without reading LED colours.
// Ported from the Overcooked_test prototype's FryingPanSimonSays.cpp move
// icons, driven here by the same server messages the LED strip gets (no
// protocol change): SetDisplay(PatternCue, patternId [+6 if time is short]).
// Everything else (Idle/Burnt/GameOver, the Success/Reject flashes, the
// progress bar) is drawn generically off DisplaySink's own calls.
//
// First guess like the rest of overcooked_pan's wiring: resolution, pins and
// layout are easy to change here if the real panel differs.

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#include <DisplaySink.h>
#include <OvercookedComm.h>

namespace pan {

class PatternOled : public station::DisplaySink {
 public:
  static constexpr uint8_t kWidth = 128;
  static constexpr uint8_t kHeight = 64;
  static constexpr uint8_t kAddress = 0x3C;
  static constexpr uint8_t kAltAddress = 0x3D; // some clones ship as this

  PatternOled(uint8_t sdaPin, uint8_t sclPin, TwoWire &wire = Wire)
      : sdaPin_(sdaPin), sclPin_(sclPin), wire_(wire), display_(kWidth, kHeight, &wire_, -1) {}

  void begin() override {
    wire_.begin(sdaPin_, sclPin_);
    // Adafruit_SSD1306::begin() reports success even with nothing on the bus,
    // so check for an ACK ourselves, on the usual address then the alternate.
    uint8_t address = 0;
    for (uint8_t candidate : {kAddress, kAltAddress}) {
      if (answers(candidate)) {
        address = candidate;
        break;
      }
    }
    if (!address) {
      Serial.printf("Pan OLED: nothing answered at 0x%02X or 0x%02X on SDA=%u SCL=%u. Devices on the bus:",
                    kAddress, kAltAddress, sdaPin_, sclPin_);
      bool any = false;
      for (uint8_t a = 1; a < 127; a++) {
        if (answers(a)) {
          Serial.printf(" 0x%02X", a);
          any = true;
        }
      }
      Serial.println(any ? "" : " none (check SDA/SCL/VCC/GND)");
      return;
    }
    ready_ = display_.begin(SSD1306_SWITCHCAPVCC, address);
    Serial.printf("Pan OLED at 0x%02X: %s\n", address, ready_ ? "ok" : "init failed");
    if (!ready_) return;
    display_.clearDisplay();
    display_.display();
  }

  void setMode(oc::DisplayMode mode, uint8_t level) override {
    mode_ = mode;
    level_ = level;
    if (mode != oc::DisplayMode::Reject && mode != oc::DisplayMode::Calibrated &&
        mode != oc::DisplayMode::Success) {
      flashing_ = false;
    }
  }

  void flash(oc::DisplayMode mode) override {
    flashing_ = true;
    flashMode_ = mode;
    flashStartedAt_ = millis();
  }

  void setProgress(uint16_t value, uint16_t goal) override {
    progressShown_ = true;
    progress_ = value;
    goal_ = goal ? goal : 1;
  }

  void clearProgress() override { progressShown_ = false; }

  void setOffline(bool offline) override { offline_ = offline; }

  void update(uint32_t now) override {
    if (!ready_ || now - lastDrawAt_ < kRenderIntervalMs) return;
    lastDrawAt_ = now;

    display_.clearDisplay();
    display_.setTextColor(SSD1306_WHITE);

    if (offline_) {
      drawCentered("NO SERVER", (kHeight - 8) / 2, 1);
    } else if (flashing_ && !drawFlash(now)) {
      flashing_ = false;
      drawMode(now);
    } else if (!flashing_) {
      drawMode(now);
    }

    display_.display();
  }

 private:
  bool answers(uint8_t address) {
    wire_.beginTransmission(address);
    return wire_.endTransmission() == 0;
  }

  static constexpr uint32_t kRenderIntervalMs = 40; // 25 Hz, plenty smooth for these animations
  static constexpr uint32_t kFlashMs = 700;
  static constexpr uint8_t kPatternCount = 4;

  // ---- one-shot flashes -----------------------------------------------------
  // Returns false once the flash has run its course.
  bool drawFlash(uint32_t now) {
    if (now - flashStartedAt_ >= kFlashMs) return false;
    switch (flashMode_) {
      case oc::DisplayMode::Success:    drawCentered("COOKED!", (kHeight - 16) / 2, 2); break;
      case oc::DisplayMode::Reject:     drawCentered("NOPE", (kHeight - 8) / 2, 1); break;
      case oc::DisplayMode::Calibrated: drawCentered("OK", (kHeight - 16) / 2, 2); break;
      default: return false;
    }
    return true;
  }

  // ---- persistent modes -------------------------------------------------------
  void drawMode(uint32_t now) {
    switch (mode_) {
      case oc::DisplayMode::PatternCue: drawPatternCue(now); break;
      case oc::DisplayMode::Burnt:      drawFire(now); break;
      case oc::DisplayMode::GameOver:   drawCentered("GAME OVER", (kHeight - 8) / 2, 1); break;
      case oc::DisplayMode::Idle:       drawIdle(); break;
      default: break;
    }
  }

  void drawIdle() {
    drawCentered("FRYING PAN", 4, 1);
    if (progressShown_) drawProgressBar(progress_, goal_, false);
  }

  void drawPatternCue(uint32_t now) {
    uint8_t pattern = level_ % kPatternCount;
    bool lowTime = level_ >= kPatternCount;
    drawCentered(patternLabel(pattern), 0, 1);
    drawMoveIcon(pattern, now);
    if (progressShown_) drawProgressBar(progress_, goal_, lowTime);
  }

  void drawFire(uint32_t now) {
    const int flameX[4] = {20, 50, 78, 108};
    for (uint8_t i = 0; i < 4; i++) {
      uint8_t phase = (now / 100 + i * 2) % 6;
      int height = 26 + (phase <= 3 ? phase : 6 - phase) * 3; // flickers ~26-35px tall
      int cx = flameX[i];
      int baseY = kHeight - 1;
      display_.fillTriangle(cx - 12, baseY, cx + 12, baseY, cx, baseY - height, SSD1306_WHITE);
    }
    drawCentered("BURNT!", 0, 1);
  }

  // ---- progress / timer bar --------------------------------------------------
  void drawProgressBar(uint16_t value, uint16_t goal, bool invert) {
    const int barY = kHeight - kBarHeight, barW = kWidth - 2;
    int filled = (int)((uint32_t)value * barW / goal);
    if (invert && (millis() / 250) % 2 == 0) return; // low-time bar blinks off, not just inverts
    display_.drawRect(0, barY, kWidth, kBarHeight, SSD1306_WHITE);
    display_.fillRect(1, barY + 1, filled, kBarHeight - 2, SSD1306_WHITE);
  }

  static constexpr uint8_t kBarHeight = 6;

  // ---- move icons, ported from FryingPanSimonSays.cpp's drawMoveIcon family --
  static constexpr int16_t kIconCx = kWidth / 2;
  static constexpr int16_t kIconCy = 30;
  static constexpr int16_t kBaseR = 15;
  static constexpr int16_t kBallR = 6;
  static constexpr int16_t kReach = 11;

  void drawBase() { display_.drawCircle(kIconCx, kIconCy, kBaseR, SSD1306_WHITE); }

  void drawMoveIcon(uint8_t pattern, uint32_t now) {
    switch (pattern) {
      case (uint8_t)oc::Pattern::Circle: drawRotateIcon(now); break;
      case (uint8_t)oc::Pattern::Zigzag: drawZigzagIcon(now); break;
      case (uint8_t)oc::Pattern::Hold:   drawHoldIcon(now); break;
      case (uint8_t)oc::Pattern::Shake:  drawShakeIcon(now); break;
      default: drawBase(); break;
    }
  }

  // Sweep round the edge: an 8-point orbit approximates a circle without sin/cos.
  void drawRotateIcon(uint32_t now) {
    drawBase();
    const int8_t orbit[8][2] = {
        {kReach, 0}, {kReach * 3 / 4, kReach * 3 / 4}, {0, kReach}, {-kReach * 3 / 4, kReach * 3 / 4},
        {-kReach, 0}, {-kReach * 3 / 4, -kReach * 3 / 4}, {0, -kReach}, {kReach * 3 / 4, -kReach * 3 / 4},
    };
    uint8_t idx = (now / 150) % 8;
    display_.fillCircle(kIconCx + orbit[idx][0], kIconCy + orbit[idx][1], kBallR, SSD1306_WHITE);
  }

  // Swing left and right: a slow, even toggle between the two sides.
  void drawZigzagIcon(uint32_t now) {
    display_.drawFastHLine(kIconCx - kBaseR, kIconCy, kBaseR * 2 + 1, SSD1306_WHITE);
    int side = ((now / 450) % 2 == 0) ? -kReach : kReach;
    display_.fillCircle(kIconCx + side, kIconCy, kBallR, SSD1306_WHITE);
  }

  // Keep it centered: a slow breathing dot reads as "stay put", not motion.
  void drawHoldIcon(uint32_t now) {
    drawBase();
    const uint8_t pulse[4] = {4, 7, 10, 7};
    display_.fillCircle(kIconCx, kIconCy, pulse[(now / 350) % 4], SSD1306_WHITE);
  }

  // Fast either-axis wiggle - the repeated motion IS the shake.
  void drawShakeIcon(uint32_t now) {
    display_.drawFastHLine(kIconCx - kBaseR, kIconCy, kBaseR * 2 + 1, SSD1306_WHITE);
    const int8_t dx[4] = {-kReach, 0, kReach, 0};
    display_.fillCircle(kIconCx + dx[(now / 130) % 4], kIconCy, kBallR, SSD1306_WHITE);
  }

  static const char *patternLabel(uint8_t pattern) {
    switch (pattern) {
      case (uint8_t)oc::Pattern::Circle: return "CIRCLE IT";
      case (uint8_t)oc::Pattern::Zigzag: return "ZIGZAG IT";
      case (uint8_t)oc::Pattern::Hold:   return "HOLD IT";
      case (uint8_t)oc::Pattern::Shake:  return "SHAKE IT";
      default: return "";
    }
  }

  void drawCentered(const char *text, int y, uint8_t size) {
    int16_t bx, by;
    uint16_t bw, bh;
    display_.setTextSize(size);
    display_.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    display_.setCursor((kWidth - (int)bw) / 2, y);
    display_.print(text);
  }

  uint8_t sdaPin_, sclPin_;
  TwoWire &wire_;
  Adafruit_SSD1306 display_;
  bool ready_ = false;

  bool offline_ = false;
  oc::DisplayMode mode_ = oc::DisplayMode::Idle;
  uint8_t level_ = 0;

  bool flashing_ = false;
  oc::DisplayMode flashMode_ = oc::DisplayMode::Idle;
  uint32_t flashStartedAt_ = 0;

  bool progressShown_ = false;
  uint16_t progress_ = 0;
  uint16_t goal_ = 1;

  uint32_t lastDrawAt_ = 0;
};

} // namespace pan
