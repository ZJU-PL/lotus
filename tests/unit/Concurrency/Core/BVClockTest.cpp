#include "Concurrency/Core/BVClock.h"
#include "Concurrency/Core/FBVClock.h"

#include <set>
#include <stdexcept>

#include <gtest/gtest.h>

TEST(BVClockTest, JoinSatisfiesBooleanLatticeLaws) {
  BVClock a;
  a.set(0);
  BVClock b;
  b.set(1);
  BVClock c;
  c.set(2);

  EXPECT_EQ(a + a, a);
  EXPECT_EQ(a + b, b + a);
  EXPECT_EQ((a + b) + c, a + (b + c));
  EXPECT_TRUE(a.leq(a + b));
  EXPECT_TRUE(b.leq(a + b));
}

TEST(BVClockTest, RvalueJoinCompilesAndLinks) {
  BVClock clock;
  clock.set(0);
  clock += BVClock{};
  EXPECT_TRUE(clock[0]);
}

TEST(BVClockTest, RejectsNegativeDimensions) {
  BVClock clock;
  EXPECT_THROW(clock.set(-1), std::out_of_range);
  EXPECT_THROW(static_cast<void>(clock[-1]), std::out_of_range);
}

TEST(BVClockTest, ContainerOrderRetainsIncomparableClocks) {
  BVClock a;
  a.set(0);
  BVClock b;
  b.set(1);
  BVClock c;
  c.set(0);
  c.set(2);

  EXPECT_FALSE(a.lt(b));
  EXPECT_FALSE(b.lt(a));

  std::set<BVClock> clocks;
  clocks.insert(a);
  clocks.insert(b);
  clocks.insert(c);
  EXPECT_EQ(clocks.size(), 3u);
}

TEST(BVClockTest, ContainerOrderCanonicalizesTrailingZeroDimensions) {
  auto system = FBVClock::new_clock_system();
  FBVClock source(system, 0);
  FBVClock unused_high_dimension(system, 3);

  BVClock compact;
  compact.set(0);
  BVClock extended;
  extended = source;

  EXPECT_EQ(compact, extended);
  EXPECT_FALSE(compact < extended);
  EXPECT_FALSE(extended < compact);

  FBVClock::delete_clock_system(system);
}

TEST(BVClockTest, FBVClockConversionCapturesCompleteClosure) {
  auto system = FBVClock::new_clock_system();
  FBVClock a(system, 0);
  FBVClock b(system, 1);
  FBVClock c(system, 2);
  a += b;
  b += c;

  BVClock assigned;
  assigned = a;
  EXPECT_TRUE(assigned[0]);
  EXPECT_TRUE(assigned[1]);
  EXPECT_TRUE(assigned[2]);

  BVClock joined;
  joined.set(3);
  joined += a;
  EXPECT_TRUE(joined[0]);
  EXPECT_TRUE(joined[1]);
  EXPECT_TRUE(joined[2]);
  EXPECT_TRUE(joined[3]);

  FBVClock::delete_clock_system(system);
}
