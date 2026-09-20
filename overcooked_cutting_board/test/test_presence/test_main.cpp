#include <unity.h>

#include <PresenceTracker.h>

using tagreader::Change;
using tagreader::PresenceTracker;

static const uint8_t kA[] = {0xDE, 0xAD, 0xBE, 0xEF};
static const uint8_t kB[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

void setUp() {}
void tearDown() {}

void test_placed_once_then_quiet() {
  PresenceTracker t(3);
  Change c = t.update(kA, 4);
  TEST_ASSERT_TRUE(c.placed);
  TEST_ASSERT_FALSE(c.removed);
  TEST_ASSERT_EQUAL_UINT8(4, c.placedUid.size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kA, c.placedUid.bytes, 4);

  for (int i = 0; i < 10; i++) {
    c = t.update(kA, 4);
    TEST_ASSERT_FALSE(c.placed);
    TEST_ASSERT_FALSE(c.removed);
  }
  TEST_ASSERT_TRUE(t.present());
}

void test_removed_only_after_miss_limit() {
  PresenceTracker t(3);
  t.update(kA, 4);
  TEST_ASSERT_FALSE(t.update(nullptr, 0).removed);
  TEST_ASSERT_FALSE(t.update(nullptr, 0).removed);
  Change c = t.update(nullptr, 0);
  TEST_ASSERT_TRUE(c.removed);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kA, c.removedUid.bytes, 4);
  TEST_ASSERT_FALSE(t.present());
  TEST_ASSERT_FALSE(t.update(nullptr, 0).removed); // reported once
}

void test_a_seen_poll_resets_the_miss_count() {
  PresenceTracker t(3);
  t.update(kA, 4);
  t.update(nullptr, 0);
  t.update(nullptr, 0);
  TEST_ASSERT_FALSE(t.update(kA, 4).placed); // blip, still the same tag
  t.update(nullptr, 0);
  TEST_ASSERT_FALSE(t.update(nullptr, 0).removed); // only 2 misses since the blip
  TEST_ASSERT_TRUE(t.present());
}

void test_replacing_a_tag_reports_removed_then_placed() {
  PresenceTracker t(3);
  t.update(kA, 4);
  Change c = t.update(kB, 7);
  TEST_ASSERT_TRUE(c.removed);
  TEST_ASSERT_TRUE(c.placed);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kA, c.removedUid.bytes, 4);
  TEST_ASSERT_EQUAL_UINT8(7, c.placedUid.size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kB, c.placedUid.bytes, 7);
}

void test_same_tag_can_be_placed_again_after_removal() {
  PresenceTracker t(2);
  t.update(kA, 4);
  t.update(nullptr, 0);
  TEST_ASSERT_TRUE(t.update(nullptr, 0).removed);
  TEST_ASSERT_TRUE(t.update(kA, 4).placed);
}

void test_nothing_seen_when_empty() {
  PresenceTracker t(3);
  Change c = t.update(nullptr, 0);
  TEST_ASSERT_FALSE(c.placed);
  TEST_ASSERT_FALSE(c.removed);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_placed_once_then_quiet);
  RUN_TEST(test_removed_only_after_miss_limit);
  RUN_TEST(test_a_seen_poll_resets_the_miss_count);
  RUN_TEST(test_replacing_a_tag_reports_removed_then_placed);
  RUN_TEST(test_same_tag_can_be_placed_again_after_removal);
  RUN_TEST(test_nothing_seen_when_empty);
  return UNITY_END();
}
