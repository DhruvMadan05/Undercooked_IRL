#include <unity.h>

#include <FryTracker.h>

using namespace fryer;

void setUp() {}
void tearDown() {}

void test_overlaps_inside_and_outside_the_zone() {
  TEST_ASSERT_TRUE(overlaps(0.5f, 0.5f, 0.3f));   // dead center
  TEST_ASSERT_TRUE(overlaps(0.60f, 0.5f, 0.3f));  // just inside (d=0.10 <= 0.15)
  TEST_ASSERT_FALSE(overlaps(0.70f, 0.5f, 0.3f)); // just outside (d=0.20 > 0.15)
  TEST_ASSERT_TRUE(overlaps(0.4f, 0.5f, 0.3f));   // symmetric on the other side (d=0.10 <= 0.15)
}

void test_progress_starts_with_a_small_head_start() {
  ProgressTracker t;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kStartProgress, t.progress());
  TEST_ASSERT_FALSE(t.done());
}

void test_progress_fills_while_overlapping() {
  ProgressTracker t;
  t.update(true, 5.0f, false); // 5s at 1/10 per sec
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kStartProgress + 0.5f, t.progress());
}

void test_progress_drains_while_missing() {
  ProgressTracker t;
  t.reset(1.0f);
  t.update(false, 30.0f, false); // 30s at 1/60 per sec
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, t.progress());
}

void test_progress_clamps_to_0_and_1() {
  ProgressTracker t;
  t.reset(0.05f);
  t.update(false, 100.0f, false);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, t.progress());

  t.update(true, 100.0f, false);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, t.progress());
  TEST_ASSERT_TRUE(t.done());
}

void test_paused_does_not_change_progress() {
  ProgressTracker t;
  float before = t.progress();
  t.update(true, 10.0f, true);
  t.update(false, 10.0f, true);
  TEST_ASSERT_EQUAL_FLOAT(before, t.progress());
}

void test_reset_resumes_at_a_saved_fraction() {
  ProgressTracker t;
  t.reset(0.4f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.4f, t.progress());
  TEST_ASSERT_FALSE(t.done());
}

void test_reset_clamps_out_of_range_fractions() {
  ProgressTracker t;
  t.reset(-1.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, t.progress());
  t.reset(5.0f);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, t.progress());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_overlaps_inside_and_outside_the_zone);
  RUN_TEST(test_progress_starts_with_a_small_head_start);
  RUN_TEST(test_progress_fills_while_overlapping);
  RUN_TEST(test_progress_drains_while_missing);
  RUN_TEST(test_progress_clamps_to_0_and_1);
  RUN_TEST(test_paused_does_not_change_progress);
  RUN_TEST(test_reset_resumes_at_a_saved_fraction);
  RUN_TEST(test_reset_clamps_out_of_range_fractions);
  return UNITY_END();
}
