#include <math.h>
#include <unity.h>

#include <ScrubTracker.h>

using namespace scrub;

void setUp() {}
void tearDown() {}

// Sweeps the stick round a circle of the given radius, stepPerSample radians at a
// time, one 20 ms sample each, and returns the tracker's active milliseconds.
static uint32_t circle(ScrubTracker &t, float radius, float stepPerSample, int samples, float startAngle = 0) {
  float a = startAngle;
  for (int i = 0; i < samples; i++) {
    t.update(radius * cosf(a), radius * sinf(a), 20);
    a += stepPerSample;
  }
  return t.activeMs();
}

void test_circling_counts_every_sample_after_the_first() {
  ScrubTracker t;
  // The first reading only sets the angle; the other 49 each moved 0.2 rad.
  TEST_ASSERT_EQUAL_UINT32(49 * 20, circle(t, 1500, 0.2f, 50));
}

void test_either_direction_counts() {
  ScrubTracker t;
  TEST_ASSERT_EQUAL_UINT32(49 * 20, circle(t, 1500, -0.2f, 50));
}

void test_a_full_turn_across_the_plus_minus_pi_wrap_is_not_a_jump() {
  ScrubTracker t;
  // Start just below pi and cross it: the raw angle jumps from +pi to -pi, the real movement is small.
  circle(t, 1500, 0.1f, 20, kPi - 0.3f);
  TEST_ASSERT_EQUAL_UINT32(19 * 20, t.activeMs());
}

void test_holding_the_stick_to_one_side_does_not_count() {
  ScrubTracker t;
  for (int i = 0; i < 100; i++) t.update(1500, 0, 20);
  TEST_ASSERT_EQUAL_UINT32(0, t.activeMs());
}

void test_slow_drift_does_not_count() {
  ScrubTracker t;
  TEST_ASSERT_EQUAL_UINT32(0, circle(t, 1500, 0.02f, 100)); // 0.02 rad < kMinDeltaAngle
}

void test_the_dead_zone_ignores_a_resting_stick() {
  ScrubTracker t;
  // Wobbles fast around the centre, but never past kDeadzoneRadius.
  TEST_ASSERT_EQUAL_UINT32(0, circle(t, kDeadzoneRadius - 50, 0.5f, 100));
}

void test_crossing_the_dead_zone_forgets_the_angle() {
  ScrubTracker t;
  t.update(1500, 0, 20);   // right
  t.update(0, 0, 20);      // through the middle
  t.update(-1500, 0, 20);  // left: 180 degrees away, but not a scrub step
  TEST_ASSERT_EQUAL_UINT32(0, t.activeMs());
  t.update(-1500 * cosf(0.2f), -1500 * sinf(0.2f), 20); // and now a real move
  TEST_ASSERT_EQUAL_UINT32(20, t.activeMs());
}

void test_reset_resumes_and_forgets_the_last_angle() {
  ScrubTracker t;
  circle(t, 1500, 0.2f, 10);
  t.reset(3000);
  TEST_ASSERT_EQUAL_UINT32(3000, t.activeMs());
  t.update(0, 1500, 20); // first reading after a reset only sets the angle
  TEST_ASSERT_EQUAL_UINT32(3000, t.activeMs());
  t.update(-1500 * sinf(0.2f), 1500 * cosf(0.2f), 20);
  TEST_ASSERT_EQUAL_UINT32(3020, t.activeMs());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_circling_counts_every_sample_after_the_first);
  RUN_TEST(test_either_direction_counts);
  RUN_TEST(test_a_full_turn_across_the_plus_minus_pi_wrap_is_not_a_jump);
  RUN_TEST(test_holding_the_stick_to_one_side_does_not_count);
  RUN_TEST(test_slow_drift_does_not_count);
  RUN_TEST(test_the_dead_zone_ignores_a_resting_stick);
  RUN_TEST(test_crossing_the_dead_zone_forgets_the_angle);
  RUN_TEST(test_reset_resumes_and_forgets_the_last_angle);
  return UNITY_END();
}
