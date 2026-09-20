#include <initializer_list>
#include <unity.h>

#include <PatternTracker.h>

using namespace pan;

static const Direction U = Direction::Up, R = Direction::Right, D = Direction::Down, L = Direction::Left,
                       N = Direction::None;

void setUp() {}
void tearDown() {}

// Feeds a sequence and returns how many steps it counted.
static int steps(PatternTracker &t, std::initializer_list<Direction> moves) {
  int n = 0;
  for (Direction d : moves) n += t.update(d);
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
  return UNITY_END();
}
