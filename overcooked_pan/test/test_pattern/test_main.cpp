#include <initializer_list>
#include <unity.h>

#include <PatternTracker.h>

using namespace pan;

static const Direction U = Direction::Up, R = Direction::Right, D = Direction::Down, L = Direction::Left,
                       N = Direction::None;

void setUp() {}
void tearDown() {}

// Feeds a sequence (click always released) and returns how many steps it
// counted. stepMs advances a fake clock between readings; Circle/Zigzag ignore
// it, Hold/Shake/Press/Flick tests that need exact timing call t.update()
// directly instead.
static int steps(PatternTracker &t, std::initializer_list<Direction> moves, uint32_t stepMs = 10) {
  uint32_t now = 0;
  int n = 0;
  for (Direction d : moves) {
    now += stepMs;
    n += t.update(d, false, now);
  }
  return n;
}

void test_quantize() {
  TEST_ASSERT_EQUAL(N, quantize(0.1f, -0.2f, 0.6f));   // dead zone
  TEST_ASSERT_EQUAL(U, quantize(0.1f, 0.9f, 0.6f));
  TEST_ASSERT_EQUAL(D, quantize(-0.2f, -0.8f, 0.6f));
  TEST_ASSERT_EQUAL(R, quantize(0.9f, 0.3f, 0.6f));
  TEST_ASSERT_EQUAL(L, quantize(-0.7f, -0.7f + 0.05f, 0.6f)); // diagonal goes to the bigger axis
  TEST_ASSERT_EQUAL(N, quantize(0.45f, 0.45f, 0.65f));  // 0.636 < 0.65
}

void test_circle_clockwise_full_loop() {
  PatternTracker t;
  t.reset(kCircle);
  TEST_ASSERT_EQUAL(4, steps(t, {U, R, D, L}));
  TEST_ASSERT_EQUAL(4, steps(t, {U, R, D, L})); // and keeps going
}

void test_circle_can_start_anywhere_and_go_counter_clockwise() {
  PatternTracker t;
  t.reset(kCircle);
  TEST_ASSERT_EQUAL(4, steps(t, {D, R, U, L}));
}

void test_circle_ignores_wobble_and_going_back() {
  PatternTracker t;
  t.reset(kCircle);
  TEST_ASSERT_EQUAL(2, steps(t, {U, R}));     // clockwise decided
  TEST_ASSERT_EQUAL(0, steps(t, {U, R, U}));  // back and forth across the boundary
  TEST_ASSERT_EQUAL(1, steps(t, {D}));        // ... the expected next one still counts
}

void test_circle_ignores_jumps_and_repeats() {
  PatternTracker t;
  t.reset(kCircle);
  TEST_ASSERT_EQUAL(1, steps(t, {U, U, U}));   // holding a direction counts once
  TEST_ASSERT_EQUAL(0, steps(t, {D}));         // straight across: skipped R
  TEST_ASSERT_EQUAL(1, steps(t, {N, R}));      // passing the centre changes nothing
}

void test_circle_reset_forgets_direction() {
  PatternTracker t;
  t.reset(kCircle);
  steps(t, {U, R});
  t.reset(kCircle);
  TEST_ASSERT_EQUAL(3, steps(t, {U, L, D}));  // now counter-clockwise is fine
}

void test_zigzag() {
  PatternTracker t;
  t.reset(kZigzag);
  TEST_ASSERT_EQUAL(4, steps(t, {L, N, R, N, L, N, R}));
  TEST_ASSERT_EQUAL(0, steps(t, {R, R}));       // same side again
  TEST_ASSERT_EQUAL(0, steps(t, {U, D, N}));    // up and down do not count
  TEST_ASSERT_EQUAL(1, steps(t, {L}));
}

void test_zigzag_starts_on_either_side() {
  PatternTracker t;
  t.reset(kZigzag);
  TEST_ASSERT_EQUAL(1, steps(t, {R}));
  t.reset(kZigzag);
  TEST_ASSERT_EQUAL(0, steps(t, {U}));
}

void test_hold_counts_after_dwell() {
  PatternTracker t;
  t.reset(kHold);
  TEST_ASSERT_EQUAL(0, t.update(N, false, 100));
  TEST_ASSERT_EQUAL(0, t.update(N, false, 400)); // 300ms in, short of the 500ms step
  TEST_ASSERT_EQUAL(1, t.update(N, false, 650)); // 550ms since first centered
}

void test_hold_repeats_without_releasing() {
  PatternTracker t;
  t.reset(kHold);
  t.update(N, false, 0);
  TEST_ASSERT_EQUAL(1, t.update(N, false, 500));
  TEST_ASSERT_EQUAL(0, t.update(N, false, 800));
  TEST_ASSERT_EQUAL(1, t.update(N, false, 1000)); // another full 500ms since the last step
}

void test_hold_resets_on_movement() {
  PatternTracker t;
  t.reset(kHold);
  t.update(N, false, 0);
  t.update(U, false, 300);                        // leaves center, dwell aborted
  TEST_ASSERT_EQUAL(0, t.update(N, false, 600));  // re-centered, dwell restarts here
  TEST_ASSERT_EQUAL(1, t.update(N, false, 1100)); // 500ms after re-centering at 600
}

void test_hold_reset_forgets_dwell() {
  PatternTracker t;
  t.reset(kHold);
  t.update(N, false, 0);
  t.update(N, false, 400);
  t.reset(kHold);
  TEST_ASSERT_EQUAL(0, t.update(N, false, 450)); // dwell restarted by reset
  TEST_ASSERT_EQUAL(1, t.update(N, false, 950));
}

void test_shake_counts_fast_alternation() {
  PatternTracker t;
  t.reset(kShake);
  TEST_ASSERT_EQUAL(0, t.update(L, false, 0)); // first edge: nothing to alternate from yet
  TEST_ASSERT_EQUAL(1, t.update(R, false, 150));
  TEST_ASSERT_EQUAL(1, t.update(L, false, 300));
  TEST_ASSERT_EQUAL(1, t.update(R, false, 450));
}

void test_shake_ignores_slow_alternation() {
  PatternTracker t;
  t.reset(kShake);
  t.update(L, false, 0);
  TEST_ASSERT_EQUAL(0, t.update(R, false, 900)); // gap too long (> 400ms window)
  TEST_ASSERT_EQUAL(1, t.update(L, false, 950)); // but back to fast from here
}

void test_shake_works_on_either_axis() {
  PatternTracker t;
  t.reset(kShake);
  t.update(U, false, 0);
  TEST_ASSERT_EQUAL(1, t.update(D, false, 100));
}

void test_shake_ignores_non_opposite_edges() {
  PatternTracker t;
  t.reset(kShake);
  t.update(U, false, 0);
  TEST_ASSERT_EQUAL(0, t.update(R, false, 100)); // adjacent, not opposite - not a shake
}

void test_shake_reset_forgets_last_direction() {
  PatternTracker t;
  t.reset(kShake);
  t.update(L, false, 0);
  t.update(R, false, 100);
  t.reset(kShake);
  TEST_ASSERT_EQUAL(0, t.update(L, false, 150)); // no prior direction after reset
}

void test_press_counts_after_hold() {
  PatternTracker t;
  t.reset(kPress);
  TEST_ASSERT_EQUAL(0, t.update(N, true, 100));
  TEST_ASSERT_EQUAL(0, t.update(N, true, 300)); // 200ms in, short of the 250ms hold
  TEST_ASSERT_EQUAL(1, t.update(N, true, 360)); // 260ms since the click went down
}

void test_press_does_not_repeat_while_held() {
  PatternTracker t;
  t.reset(kPress);
  t.update(N, true, 0);
  TEST_ASSERT_EQUAL(1, t.update(N, true, 250));
  TEST_ASSERT_EQUAL(0, t.update(N, true, 260)); // still held, already counted
  TEST_ASSERT_EQUAL(0, t.update(N, true, 1000)); // held indefinitely, still just the one
}

void test_press_needs_release_before_next() {
  PatternTracker t;
  t.reset(kPress);
  t.update(N, true, 0);
  t.update(N, true, 250);
  TEST_ASSERT_EQUAL(0, t.update(N, false, 260)); // released
  TEST_ASSERT_EQUAL(0, t.update(N, true, 300));  // pressed again, dwell restarts here
  TEST_ASSERT_EQUAL(1, t.update(N, true, 560));  // 260ms since this second press started
}

void test_press_ignores_a_short_tap() {
  PatternTracker t;
  t.reset(kPress);
  t.update(N, true, 0);
  TEST_ASSERT_EQUAL(0, t.update(N, false, 100)); // released well under 250ms
}

void test_flick_counts_click_and_up_together() {
  PatternTracker t;
  t.reset(kFlick);
  TEST_ASSERT_EQUAL(1, t.update(U, true, 0)); // both start on the same reading
}

void test_flick_allows_a_small_gap_either_order() {
  PatternTracker t;
  t.reset(kFlick);
  t.update(U, false, 0);              // stick goes up first
  TEST_ASSERT_EQUAL(1, t.update(U, true, 150)); // click follows 150ms later, within the window
}

void test_flick_ignores_a_stale_click() {
  PatternTracker t;
  t.reset(kFlick);
  t.update(N, true, 0);               // click held well before any push
  TEST_ASSERT_EQUAL(0, t.update(U, true, 500)); // stick only goes up 500ms later
}

void test_flick_does_not_repeat_while_both_held() {
  PatternTracker t;
  t.reset(kFlick);
  TEST_ASSERT_EQUAL(1, t.update(U, true, 0));
  TEST_ASSERT_EQUAL(0, t.update(U, true, 50)); // still both active, already counted
}

void test_flick_rearms_after_release() {
  PatternTracker t;
  t.reset(kFlick);
  t.update(U, true, 0);
  t.update(N, false, 50); // both let go
  TEST_ASSERT_EQUAL(1, t.update(U, true, 100));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_quantize);
  RUN_TEST(test_circle_clockwise_full_loop);
  RUN_TEST(test_circle_can_start_anywhere_and_go_counter_clockwise);
  RUN_TEST(test_circle_ignores_wobble_and_going_back);
  RUN_TEST(test_circle_ignores_jumps_and_repeats);
  RUN_TEST(test_circle_reset_forgets_direction);
  RUN_TEST(test_zigzag);
  RUN_TEST(test_zigzag_starts_on_either_side);
  RUN_TEST(test_hold_counts_after_dwell);
  RUN_TEST(test_hold_repeats_without_releasing);
  RUN_TEST(test_hold_resets_on_movement);
  RUN_TEST(test_hold_reset_forgets_dwell);
  RUN_TEST(test_shake_counts_fast_alternation);
  RUN_TEST(test_shake_ignores_slow_alternation);
  RUN_TEST(test_shake_works_on_either_axis);
  RUN_TEST(test_shake_ignores_non_opposite_edges);
  RUN_TEST(test_shake_reset_forgets_last_direction);
  RUN_TEST(test_press_counts_after_hold);
  RUN_TEST(test_press_does_not_repeat_while_held);
  RUN_TEST(test_press_needs_release_before_next);
  RUN_TEST(test_press_ignores_a_short_tap);
  RUN_TEST(test_flick_counts_click_and_up_together);
  RUN_TEST(test_flick_allows_a_small_gap_either_order);
  RUN_TEST(test_flick_ignores_a_stale_click);
  RUN_TEST(test_flick_does_not_repeat_while_both_held);
  RUN_TEST(test_flick_rearms_after_release);
  return UNITY_END();
}
