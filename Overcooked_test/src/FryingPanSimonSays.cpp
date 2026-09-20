#include "FryingPanSimonSays.h"

#include <stdio.h>
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BetterJoystick.h>

#include "FryingPanPins.h"

// TODO: everything in this file (deadzone, gesture timings, cook/burn
// durations, chain bonus curve) was tuned by hand/guesswork before the
// physical frying pan prop existed. Once the real joystick and pan are
// mounted together, recalibrate against the actual hardware feel and expect
// to revise the moves/thresholds - what worked on a bare dev board won't
// necessarily hold once it's mechanically assembled.

namespace {

// Two panels on separate I2C buses: moveDisplay shows only the move animation,
// barDisplay shows only the timer bar. See FryingPanPins.h for the wiring.
Adafruit_SSD1306 moveDisplay(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_SSD1306 barDisplay(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, -1);

// The bar screen is optional: if it isn't wired up (or fails to init) the game
// still runs on the move screen alone, which also carries its own compact bar.
bool barDisplayReady = false;

// Better Joystick assumes a 10-bit ADC (0-1023, center 512). ESP32 defaults
// to 12-bit, so analogReadResolution(10) is set in fryingPanSetup() to match it.
Joystick stick(JOYSTICK_X_PIN, JOYSTICK_Y_PIN, JOYSTICK_CLICK_PIN);

// ---------- Tunables (see README "Customization Options") ----------
#define COOK_TIME_MS         30000UL // cook timer counts down from this in real time
#define CHAIN_BONUS_STEP_MS  500UL   // each move in a correct-move chain removes this much more...
#define CHAIN_BONUS_CAP_MS   3000UL  // ...capped at this much per move
#define MISS_INDICATOR_MS    250UL   // how long the small "wrong move" mark stays on screen
#define CORRECT_INDICATOR_MS 250UL   // how long the small "correct move" mark stays on screen
#define READY_BURN_TIMEOUT_MS 10000UL // grace period to pick it up before it burns, once cooked
#define WRONG_MOVES_TO_BURN  10       // cumulative wrong moves (not necessarily in a row) before it catches fire

#define HOLD_STEADY_MS       500UL
#define PRESS_HOLD_MS        250UL
#define SHAKE_ALTERNATIONS   3       // direction edges needed (3 = one full back-and-forth), forgiving on purpose
#define SHAKE_WINDOW_MS      1000UL  // those edges must all land within this, or it isn't a shake
#define ROTATE_EVENTS        4       // 4 edges = one full lap around the four directions
#define GESTURE_WINDOW_MS    1000UL  // max time a shake/rotate gesture has to complete
#define PLAIN_CONFIRM_MS     130UL   // settle time before a single push is confirmed
#define MULTI_EDGE_CONFIRM_MS 320UL  // longer settle once a multi-direction gesture is clearly underway
#define FLICK_RELEASE_GRACE_MS 250UL // a flick landing just after the click releases still counts
#define SETTLE_MS            90UL    // stick must rest at center this long before the next move is judged

#define MAX_GESTURE_EVENTS   10      // capacity of gestureBuf

// Input is polled far faster than the screens are redrawn. Pushing a 128x32
// frame over I2C takes ~10ms per panel, and when input shared that cadence
// (20Hz) whole gesture edges fell between samples and moves went unregistered.
#define INPUT_INTERVAL_MS    10UL    // 100Hz - fast enough to catch a quick flick
#define RENDER_INTERVAL_MS   40UL    // 25Hz - plenty smooth for these animations

// ---------- Moves ----------
enum Move : uint8_t {
  MOVE_UP = 0, MOVE_DOWN, MOVE_LEFT, MOVE_RIGHT,
  MOVE_HOLD, MOVE_SHAKE, MOVE_ROTATE, MOVE_PRESS, MOVE_FLICK
};

// Serial only - makes "why didn't my shake register?" answerable from the monitor.
const char *MOVE_NAME[] = {
  "UP", "DOWN", "LEFT", "RIGHT", "HOLD", "SHAKE", "ROTATE", "PRESS", "FLICK"
};

enum Direction : uint8_t { DIR_NONE, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

// ---------- Game state ----------
enum GameState { STATE_MENU, STATE_COOKING, STATE_READY, STATE_DONE, STATE_FIRE };

GameState gameState = STATE_MENU;

Move currentMove = MOVE_UP;
uint16_t movesCompleted = 0;

// The cook timer is real time (COOK_TIME_MS) minus every bonus earned so far.
unsigned long cookStartTime = 0;
unsigned long timeBonusMs = 0;
uint8_t chainCount = 0; // consecutive correct moves; resets to 0 on a miss
uint8_t wrongMoveTotal = 0; // cumulative misses over the whole cook, never reset by the chain

unsigned long readyStartTime = 0; // when STATE_READY started, for the burn-grace countdown

unsigned long missIndicatorUntil = 0;    // small on-screen mark shown until this time
unsigned long correctIndicatorUntil = 0; // small on-screen mark shown until this time

unsigned long lastInputPoll = 0;
unsigned long lastRender = 0;

// ---------- Input / gesture detection ----------
struct GestureEvent { Direction dir; unsigned long time; };
GestureEvent gestureBuf[MAX_GESTURE_EVENTS];
uint8_t gestureCount = 0;
unsigned long gestureLastEdgeTime = 0;
Direction lastRawDir = DIR_NONE;

bool holdArmed = true; // re-armed once the stick leaves center after a hold fires
unsigned long holdStartTime = 0;

bool clickWasPressed = false;
unsigned long clickPressStart = 0;
bool clickGestureHandled = false;
unsigned long flickGraceUntil = 0; // short window after click release where an UP still counts as FLICK

bool inputBusy = false; // universal cooldown after a move is registered
unsigned long neutralSince = 0; // when the stick first went neutral during that cooldown

// menu / done-screen click edge tracking
bool menuClickWasPressed = false;

Direction dirToDirection(int dx, int dy) {
  if (abs(dx) < JOYSTICK_DEADZONE && abs(dy) < JOYSTICK_DEADZONE) return DIR_NONE;
  if (abs(dx) > abs(dy)) return dx > 0 ? DIR_RIGHT : DIR_LEFT;
  return dy > 0 ? DIR_UP : DIR_DOWN; // assumes higher Y reading = pushed up; invert here if wiring differs
}

Move directionToMove(Direction d) {
  switch (d) {
    case DIR_UP: return MOVE_UP;
    case DIR_DOWN: return MOVE_DOWN;
    case DIR_LEFT: return MOVE_LEFT;
    default: return MOVE_RIGHT;
  }
}

bool isOpposite(Direction a, Direction b) {
  return (a == DIR_LEFT && b == DIR_RIGHT) || (a == DIR_RIGHT && b == DIR_LEFT) ||
         (a == DIR_UP && b == DIR_DOWN) || (a == DIR_DOWN && b == DIR_UP);
}

void clearGestureBuf() {
  gestureCount = 0;
}

void generateNextMove() {
  Move m;
  do {
    m = (Move)random(0, MOVE_FLICK + 1);
  } while (m == currentMove);
  currentMove = m;
}

long timeRemainingMs() {
  long remaining = (long)COOK_TIME_MS - (long)(millis() - cookStartTime) - (long)timeBonusMs;
  return remaining > 0 ? remaining : 0;
}

void startCooking() {
  cookStartTime = millis();
  timeBonusMs = 0;
  chainCount = 0;
  wrongMoveTotal = 0;
  movesCompleted = 0;
  missIndicatorUntil = 0;
  correctIndicatorUntil = 0;
  generateNextMove();
  clearGestureBuf();
  holdArmed = true;
  inputBusy = false;
  neutralSince = 0;
  lastRawDir = DIR_NONE;
  gameState = STATE_COOKING;
}

void registerMove(Move m) {
  if (gameState != STATE_COOKING) return;

  inputBusy = true;
  Serial.printf("read %-6s want %-6s %s\n", MOVE_NAME[m], MOVE_NAME[currentMove],
                m == currentMove ? "OK" : "miss");

  if (m != currentMove) {
    // Wrong moves aren't penalized on the timer - they just break the bonus
    // chain and show a brief, small mark. But they're still counted: too many
    // of them (even spread out over the whole cook) sets the pan on fire.
    chainCount = 0;
    missIndicatorUntil = millis() + MISS_INDICATOR_MS;
    wrongMoveTotal++;
    if (wrongMoveTotal >= WRONG_MOVES_TO_BURN) {
      gameState = STATE_FIRE;
    }
    return;
  }

  unsigned long bonus = min(CHAIN_BONUS_CAP_MS, CHAIN_BONUS_STEP_MS * (chainCount + 1));
  timeBonusMs += bonus;
  chainCount++;
  movesCompleted++;
  correctIndicatorUntil = millis() + CORRECT_INDICATOR_MS;
  generateNextMove();
}

bool isShakePattern() {
  // Strict alternation over just the last SHAKE_ALTERNATIONS edges, so a
  // single back-and-forth (3 edges: e.g. L,R,L) is enough - no need to wait
  // for a second full cycle like the old 4-edge check required. The whole run
  // also has to land inside SHAKE_WINDOW_MS, so slow, unrelated pushes that
  // happen to alternate don't accumulate into a "shake".
  if (gestureCount < SHAKE_ALTERNATIONS) return false;
  uint8_t first = gestureCount - SHAKE_ALTERNATIONS;
  for (uint8_t i = first + 1; i < gestureCount; i++) {
    if (!isOpposite(gestureBuf[i].dir, gestureBuf[i - 1].dir)) return false;
  }
  return gestureBuf[gestureCount - 1].time - gestureBuf[first].time <= SHAKE_WINDOW_MS;
}

void processGestureBuffer() {
  if (gestureCount == 0) return;

  if (isShakePattern()) {
    registerMove(MOVE_SHAKE);
    clearGestureBuf();
    return;
  }

  if (gestureCount >= ROTATE_EVENTS) {
    registerMove(MOVE_ROTATE);
    clearGestureBuf();
    return;
  }

  unsigned long elapsedSinceEdge = millis() - gestureLastEdgeTime;
  unsigned long elapsedSinceStart = millis() - gestureBuf[0].time;

  // Once the stick has already changed direction more than once the player is
  // clearly mid-rotate/shake, so wait longer before giving up and calling it a
  // plain push. A single push still resolves at the snappy PLAIN_CONFIRM_MS.
  unsigned long confirmMs = (gestureCount >= 2) ? MULTI_EDGE_CONFIRM_MS : PLAIN_CONFIRM_MS;

  if (elapsedSinceEdge >= confirmMs || elapsedSinceStart >= GESTURE_WINDOW_MS) {
    registerMove(directionToMove(gestureBuf[0].dir));
    clearGestureBuf();
  }
}

void handleInput() {
  int dx = stick.x() - 512;
  int dy = stick.y() - 512;
  Direction rawDir = dirToDirection(dx, dy);
  bool clicked = stick.isPressed();

  if (inputBusy) {
    // Grace period after finishing a move: the stick has to actually come to
    // rest, not just blip through center. A shake in particular keeps swinging
    // after it's been recognized, and without this the leftover motion would
    // get judged as a (wrong) attempt at the move that just came up.
    if (rawDir == DIR_NONE && !clicked) {
      if (neutralSince == 0) {
        neutralSince = millis();
      } else if (millis() - neutralSince >= SETTLE_MS) {
        inputBusy = false;
        neutralSince = 0;
        lastRawDir = DIR_NONE;
        clearGestureBuf();
        holdStartTime = 0;
        flickGraceUntil = 0;
        clickWasPressed = false;
        clickGestureHandled = false;
      }
    } else {
      neutralSince = 0; // moved again before settling - restart the grace window
    }
    return;
  }

  // Click handling takes priority: reserve the joystick for flick-up detection only.
  if (clicked) {
    if (!clickWasPressed) {
      clickWasPressed = true;
      clickPressStart = millis();
      clickGestureHandled = false;
      clearGestureBuf();
      lastRawDir = DIR_NONE;
      // Click and flick landing on the very same tick still counts.
      if (rawDir == DIR_UP) {
        registerMove(MOVE_FLICK);
        clickGestureHandled = true;
      }
    } else if (!clickGestureHandled) {
      if (rawDir == DIR_UP) {
        registerMove(MOVE_FLICK);
        clickGestureHandled = true;
      } else if (millis() - clickPressStart >= PRESS_HOLD_MS) {
        registerMove(MOVE_PRESS);
        clickGestureHandled = true;
      }
    }
    return;
  }
  // Click just released without resolving to a press/flick yet - the flick's
  // peak often lands just after the button springs back, so give it a
  // brief grace window instead of requiring the two to overlap exactly.
  if (clickWasPressed && !clickGestureHandled) {
    flickGraceUntil = millis() + FLICK_RELEASE_GRACE_MS;
  }
  clickWasPressed = false;

  if (millis() < flickGraceUntil) {
    if (rawDir == DIR_UP) {
      registerMove(MOVE_FLICK);
      flickGraceUntil = 0;
      return;
    }
  } else {
    flickGraceUntil = 0;
  }

  // Hold steady: only while the stick has been resting at center with no pending gesture.
  if (rawDir == DIR_NONE && gestureCount == 0) {
    if (holdArmed) {
      if (holdStartTime == 0) {
        holdStartTime = millis();
      } else if (millis() - holdStartTime >= HOLD_STEADY_MS) {
        registerMove(MOVE_HOLD);
        holdStartTime = 0;
        holdArmed = false;
      }
    }
    return;
  }
  holdStartTime = 0;
  if (rawDir != DIR_NONE) holdArmed = true;

  if (rawDir != lastRawDir) {
    lastRawDir = rawDir;
    // Skip an edge that repeats the previous recorded direction. Dipping back
    // through center mid-push (or ADC noise near the deadzone) would otherwise
    // log the same direction twice and inflate the count into a false ROTATE.
    bool repeatsLast = gestureCount > 0 && gestureBuf[gestureCount - 1].dir == rawDir;
    if (rawDir != DIR_NONE && !repeatsLast && gestureCount < MAX_GESTURE_EVENTS) {
      gestureBuf[gestureCount].dir = rawDir;
      gestureBuf[gestureCount].time = millis();
      gestureCount++;
      gestureLastEdgeTime = millis();
    }
  }

  processGestureBuffer();
}

void checkCookState() {
  if (gameState == STATE_COOKING) {
    if (timeRemainingMs() <= 0) {
      // Cooked through - now there's a grace period to grab it before it burns.
      gameState = STATE_READY;
      readyStartTime = millis();
    }
  } else if (gameState == STATE_READY) {
    if (millis() - readyStartTime >= READY_BURN_TIMEOUT_MS) {
      gameState = STATE_FIRE;
    }
  }
}

// ---------- Move icon animations (move screen) ----------
// Every move is an animated "joystick ball" rather than a static letter/arrow,
// so the motion to perform reads at a glance. The icon sits above the move
// screen's own compact timer bar, so it's centered on that upper band rather
// than on the panel as a whole.
const int MOVE_BAR_Y  = 26; // compact bar along the bottom of the move screen
const int MOVE_BAR_H  = 6;
const int ICON_CX     = SCREEN_WIDTH / 2;
const int ICON_CY     = 12;
const int ICON_BASE_R = 9;
const int ICON_BALL_R = 4;
const int ICON_REACH  = 6; // how far the ball travels from center on a push/orbit
const int FLICK_RISE  = 8; // flick snaps further than a normal push

void drawJoystickBase() {
  moveDisplay.drawCircle(ICON_CX, ICON_CY, ICON_BASE_R, SSD1306_WHITE);
}

void drawHoldIcon() {
  drawJoystickBase();
  // Slow breathing dot - reads as "stay put" rather than motion.
  const uint8_t pulse[4] = { 3, 5, 7, 5 };
  uint8_t r = pulse[(millis() / 350) % 4];
  moveDisplay.fillCircle(ICON_CX, ICON_CY, r, SSD1306_WHITE);
}

void drawShakeIcon() {
  // Kept deliberately brisk and back-and-forth - that repeated motion IS the
  // shake, unlike the directional pushes below which now hold their position.
  moveDisplay.drawFastHLine(ICON_CX - ICON_BASE_R, ICON_CY, ICON_BASE_R * 2 + 1, SSD1306_WHITE);
  const int8_t dx[4] = { -ICON_REACH, 0, ICON_REACH, 0 };
  int offset = dx[(millis() / 160) % 4];
  moveDisplay.fillCircle(ICON_CX + offset, ICON_CY, ICON_BALL_R, SSD1306_WHITE);
}

void drawRotateIcon() {
  drawJoystickBase();
  // 8-point orbit approximates a circle without needing sin/cos each frame.
  const int8_t orbit[8][2] = {
    { 6, 0 }, { 4, 4 }, { 0, 6 }, { -4, 4 }, { -6, 0 }, { -4, -4 }, { 0, -6 }, { 4, -4 }
  };
  uint8_t idx = (millis() / 150) % 8;
  moveDisplay.fillCircle(ICON_CX + orbit[idx][0], ICON_CY + orbit[idx][1], ICON_BALL_R, SSD1306_WHITE);
}

void drawPressIcon() {
  drawJoystickBase();
  // Slow, even toggle between resting and pressed-in - a deliberate "push and
  // hold the button" rather than a rapid tap.
  bool pressed = (millis() / 500) % 2 == 1;
  int y = pressed ? ICON_CY + ICON_REACH : ICON_CY;
  moveDisplay.fillCircle(ICON_CX, y, ICON_BALL_R, SSD1306_WHITE);
  if (pressed) {
    moveDisplay.drawFastHLine(ICON_CX - 7, ICON_CY + ICON_BASE_R + 2, 14, SSD1306_WHITE);
  }
}

void drawFlickIcon() {
  drawJoystickBase();
  // Press-and-hold, then snap up. The up half draws shrinking ghost balls
  // along the path it travelled so the flick reads as a fast upward snap
  // rather than the ball just teleporting between two positions.
  if (millis() % 1400 < 800) {
    moveDisplay.fillCircle(ICON_CX, ICON_CY + ICON_REACH, ICON_BALL_R, SSD1306_WHITE);
    moveDisplay.drawFastHLine(ICON_CX - 7, ICON_CY + ICON_BASE_R + 2, 14, SSD1306_WHITE);
  } else {
    moveDisplay.fillCircle(ICON_CX, ICON_CY - FLICK_RISE, ICON_BALL_R, SSD1306_WHITE);
    moveDisplay.fillCircle(ICON_CX, ICON_CY, 3, SSD1306_WHITE);
    moveDisplay.fillCircle(ICON_CX, ICON_CY + ICON_REACH, 1, SSD1306_WHITE);
  }
}

void drawPushIcon(int8_t dirX, int8_t dirY) {
  drawJoystickBase();
  // Slow toggle between resting at center and held out toward the target
  // edge - a clear "push this way and hold," not a rapid back-and-forth
  // (which used to make every direction look like a shake).
  bool pushed = (millis() / 500) % 2 == 1;
  int offset = pushed ? ICON_REACH : 0;
  moveDisplay.fillCircle(ICON_CX + dirX * offset, ICON_CY + dirY * offset, ICON_BALL_R, SSD1306_WHITE);
}

void drawMoveIcon(Move m) {
  switch (m) {
    case MOVE_UP:     drawPushIcon(0, -1); break;
    case MOVE_DOWN:   drawPushIcon(0, 1); break;
    case MOVE_LEFT:   drawPushIcon(-1, 0); break;
    case MOVE_RIGHT:  drawPushIcon(1, 0); break;
    case MOVE_HOLD:   drawHoldIcon(); break;
    case MOVE_SHAKE:  drawShakeIcon(); break;
    case MOVE_ROTATE: drawRotateIcon(); break;
    case MOVE_PRESS:  drawPressIcon(); break;
    case MOVE_FLICK:  drawFlickIcon(); break;
    default: break;
  }
}

// ---------- Display helpers ----------
// Centers one line of text horizontally on the given panel.
void printCentered(Adafruit_SSD1306 &panel, const char *text, int y, uint8_t size) {
  int16_t bx, by; uint16_t bw, bh;
  panel.setTextSize(size);
  panel.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  panel.setCursor((SCREEN_WIDTH - (int)bw) / 2, y);
  panel.print(text);
}

float clampedFraction(long remainingMs, unsigned long totalMs) {
  float frac = (float)remainingMs / (float)totalMs;
  if (frac < 0) return 0;
  if (frac > 1) return 1;
  return frac;
}

// Compact bar along the bottom of the move screen, so the timer is still
// visible when the second panel isn't connected.
void drawMoveScreenBar(long remainingMs, unsigned long totalMs) {
  int barWidth = (int)(clampedFraction(remainingMs, totalMs) * (SCREEN_WIDTH - 2));
  moveDisplay.drawRect(0, MOVE_BAR_Y, SCREEN_WIDTH, MOVE_BAR_H, SSD1306_WHITE);
  moveDisplay.fillRect(1, MOVE_BAR_Y + 1, max(0, barWidth), MOVE_BAR_H - 2, SSD1306_WHITE);
}

// The bar panel's only job: one big progress bar with the remaining time
// printed across it (in INVERSE so the digits stay legible over both the
// filled and empty portions). Skipped entirely if that panel isn't wired up.
void drawTimerBar(long remainingMs, unsigned long totalMs) {
  if (!barDisplayReady) return;

  barDisplay.clearDisplay();

  const int barY = 4, barH = 24;
  int barWidth = (int)(clampedFraction(remainingMs, totalMs) * (SCREEN_WIDTH - 2));
  barDisplay.drawRect(0, barY, SCREEN_WIDTH, barH, SSD1306_WHITE);
  barDisplay.fillRect(1, barY + 1, max(0, barWidth), barH - 2, SSD1306_WHITE);

  char timeLabel[8];
  snprintf(timeLabel, sizeof(timeLabel), "%ld.%lds", remainingMs / 1000, (remainingMs % 1000) / 100);
  int16_t tx, ty; uint16_t tw, th;
  barDisplay.setTextSize(2);
  barDisplay.getTextBounds(timeLabel, 0, 0, &tx, &ty, &tw, &th);
  barDisplay.setTextColor(SSD1306_INVERSE);
  barDisplay.setCursor((SCREEN_WIDTH - (int)tw) / 2, barY + (barH - (int)th) / 2 - ty);
  barDisplay.print(timeLabel);
  barDisplay.setTextColor(SSD1306_WHITE);

  barDisplay.display();
}

// Text-only content for the bar panel; a no-op when it isn't connected.
void drawBarMessage(const char *line1, const char *line2, uint8_t size, bool blink) {
  if (!barDisplayReady) return;

  barDisplay.clearDisplay();
  barDisplay.setTextColor(SSD1306_WHITE);
  if (!blink || (millis() / 300) % 2 == 0) {
    if (line2) {
      printCentered(barDisplay, line1, 0, size);
      printCentered(barDisplay, line2, 16, size);
    } else {
      printCentered(barDisplay, line1, (SCREEN_HEIGHT - 8 * size) / 2, size);
    }
  }
  barDisplay.display();
}

// ---------- Per-state screens ----------
void drawMenu() {
  moveDisplay.clearDisplay();
  moveDisplay.setTextColor(SSD1306_WHITE);
  printCentered(moveDisplay, "FRYING PAN", 4, 1);
  printCentered(moveDisplay, "SIMON SAYS", 18, 1);
  moveDisplay.display();

  drawBarMessage("PRESS TO COOK", nullptr, 1, true);
}

void drawCooking() {
  moveDisplay.clearDisplay();
  moveDisplay.setTextColor(SSD1306_WHITE);

  // The move animation is the main event; a compact bar underneath keeps the
  // timer visible here too, in case the second (bar) screen isn't connected.
  drawMoveIcon(currentMove);
  long remainingMs = timeRemainingMs();
  drawMoveScreenBar(remainingMs, COOK_TIME_MS);

  // Small marks (not big banners) in opposite corners: a circle top-left for
  // a correct move, a square top-right for a wrong one that broke the chain.
  if (millis() < correctIndicatorUntil) {
    moveDisplay.fillCircle(6, 6, 5, SSD1306_WHITE);
  }
  if (millis() < missIndicatorUntil) {
    moveDisplay.fillRect(SCREEN_WIDTH - 12, 1, 11, 11, SSD1306_WHITE);
  }

  moveDisplay.display();

  drawTimerBar(remainingMs, COOK_TIME_MS);
}

void drawReady() {
  moveDisplay.clearDisplay();
  moveDisplay.setTextColor(SSD1306_WHITE);
  printCentered(moveDisplay, "COOKED!", 0, 2);
  if ((millis() / 300) % 2 == 0) {
    printCentered(moveDisplay, "TAKE IT OFF!", 17, 1);
  }

  long remain = (long)READY_BURN_TIMEOUT_MS - (long)(millis() - readyStartTime);
  if (remain < 0) remain = 0;
  drawMoveScreenBar(remain, READY_BURN_TIMEOUT_MS);

  moveDisplay.display();

  drawTimerBar(remain, READY_BURN_TIMEOUT_MS);
}

void drawDone() {
  moveDisplay.clearDisplay();
  moveDisplay.setTextColor(SSD1306_WHITE);
  printCentered(moveDisplay, "ORDER UP!", 2, 2);
  char score[20];
  snprintf(score, sizeof(score), "Moves hit: %u", movesCompleted);
  printCentered(moveDisplay, score, 22, 1);
  moveDisplay.display();

  drawBarMessage("PRESS TO COOK AGAIN", nullptr, 1, true);
}

void drawFire() {
  // Flames on the move panel, alarm text on the bar panel.
  moveDisplay.clearDisplay();
  const int flameX[4] = { 16, 48, 80, 112 };
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t phase = (millis() / 100 + i * 2) % 6;
    int height = 18 + (phase <= 3 ? phase : 6 - phase) * 2; // flickers 18-24px tall
    int cx = flameX[i];
    int baseY = SCREEN_HEIGHT - 1;
    moveDisplay.fillTriangle(cx - 9, baseY, cx + 9, baseY, cx, baseY - height, SSD1306_WHITE);
  }
  moveDisplay.display();

  drawBarMessage("FIRE FIRE", "FIRE FIRE", 2, true);
}

void updateDisplay() {
  switch (gameState) {
    case STATE_MENU:    drawMenu(); break;
    case STATE_COOKING: drawCooking(); break;
    case STATE_READY:   drawReady(); break;
    case STATE_DONE:    drawDone(); break;
    case STATE_FIRE:    drawFire(); break;
  }
}

void handleReadyInput() {
  // A click means "taken off the pan in time" - separate from handleIdleInput
  // because it advances to STATE_DONE instead of starting a new cook.
  bool clicked = stick.isPressed();
  if (clicked && !menuClickWasPressed) {
    menuClickWasPressed = true;
    gameState = STATE_DONE;
  } else if (!clicked) {
    menuClickWasPressed = false;
  }
}

void handleIdleInput() {
  bool clicked = stick.isPressed();
  if (clicked && !menuClickWasPressed) {
    menuClickWasPressed = true;
    startCooking();
  } else if (!clicked) {
    menuClickWasPressed = false;
  }
}

} // namespace

void fryingPanSetup() {
  Serial.begin(115200);

  analogReadResolution(10); // matches Better Joystick's hardcoded 0-1023 assumption

  // Separate I2C buses - both panels are at the same address (0x3C).
  Wire.begin(OLED_MOVE_SDA_PIN, OLED_MOVE_SCL_PIN);
  Wire1.begin(OLED_BAR_SDA_PIN, OLED_BAR_SCL_PIN);

  // periphBegin=false: the buses are already up with the pins above, and the
  // library's own wire->begin() takes no pin arguments.
  if (!moveDisplay.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true, false)) {
    Serial.println("Move SSD1306 init failed");
    while (true) delay(1000);
  }
  moveDisplay.clearDisplay();
  moveDisplay.display();

  // Optional - the game is fully playable on the move screen alone.
  barDisplayReady = barDisplay.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS, true, false);
  if (barDisplayReady) {
    barDisplay.clearDisplay();
    barDisplay.display();
  } else {
    Serial.println("Bar SSD1306 not found - running on the move screen only");
  }

  randomSeed(analogRead(JOYSTICK_X_PIN) + analogRead(JOYSTICK_Y_PIN) + micros());

  Serial.println("Frying Pan Simon Says ready.");
}

void fryingPanLoop() {
  unsigned long now = millis();

  // Input polls fast (100Hz) so quick gestures can't slip between samples;
  // the display only needs to redraw slow enough for smooth animation (25Hz).
  if (now - lastInputPoll >= INPUT_INTERVAL_MS) {
    lastInputPoll = now;
    if (gameState == STATE_COOKING) {
      handleInput();
      checkCookState();
    } else if (gameState == STATE_READY) {
      handleReadyInput();
      checkCookState();
    } else {
      handleIdleInput();
    }
  }

  if (now - lastRender >= RENDER_INTERVAL_MS) {
    lastRender = now;
    updateDisplay();
  }
}
