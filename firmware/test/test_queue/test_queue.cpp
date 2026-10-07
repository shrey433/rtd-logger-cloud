#include <unity.h>
#include "../../src/ReadingQueue.h"

static Reading rows[4];
static ReadingQueue q;

static Reading make(uint32_t ts) {
  Reading r{};
  r.ts = ts;
  return r;
}

void setUp() { q.begin(rows, 4); }
void tearDown() {}

void test_empty_queue_has_nothing_to_peek() {
  Reading out;
  TEST_ASSERT_FALSE(q.peekOldest(out));
  TEST_ASSERT_EQUAL(0, q.size());
}

void test_rows_come_out_oldest_first() {
  q.push(make(10));
  q.push(make(20));
  q.push(make(30));
  Reading out;
  TEST_ASSERT_TRUE(q.peekOldest(out));
  TEST_ASSERT_EQUAL_UINT32(10, out.ts);
  q.popOldest();
  TEST_ASSERT_TRUE(q.peekOldest(out));
  TEST_ASSERT_EQUAL_UINT32(20, out.ts);
  TEST_ASSERT_EQUAL(2, q.size());
}

void test_full_queue_drops_oldest_and_counts_it() {
  for (uint32_t t = 1; t <= 6; t++) q.push(make(t));
  TEST_ASSERT_EQUAL(4, q.size());
  TEST_ASSERT_EQUAL_UINT32(2, q.dropped());
  Reading out;
  q.peekOldest(out);
  TEST_ASSERT_EQUAL_UINT32(3, out.ts);
}

void test_wraparound_keeps_order() {
  Reading out;
  for (uint32_t t = 1; t <= 4; t++) q.push(make(t));
  q.popOldest();
  q.popOldest();
  q.push(make(5));
  q.push(make(6));
  uint32_t expect[] = {3, 4, 5, 6};
  for (uint32_t e : expect) {
    TEST_ASSERT_TRUE(q.peekOldest(out));
    TEST_ASSERT_EQUAL_UINT32(e, out.ts);
    q.popOldest();
  }
  TEST_ASSERT_EQUAL(0, q.size());
}

void test_zero_capacity_is_safe() {
  q.begin(rows, 0);
  q.push(make(1));
  Reading out;
  TEST_ASSERT_FALSE(q.peekOldest(out));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_empty_queue_has_nothing_to_peek);
  RUN_TEST(test_rows_come_out_oldest_first);
  RUN_TEST(test_full_queue_drops_oldest_and_counts_it);
  RUN_TEST(test_wraparound_keeps_order);
  RUN_TEST(test_zero_capacity_is_safe);
  return UNITY_END();
}
