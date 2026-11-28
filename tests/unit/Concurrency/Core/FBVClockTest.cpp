#include "Concurrency/Core/BVClock.h"
#include "Concurrency/Core/FBVClock.h"

#include <cstdint>
#include <stdexcept>
#include <type_traits>

#include <gtest/gtest.h>

static_assert(!std::is_copy_constructible<FBVClock>::value,
              "FBVClock copies would alias mutable dependency nodes");
static_assert(!std::is_copy_assignable<FBVClock>::value,
              "FBVClock copies would alias mutable dependency nodes");

TEST(FBVClockTest, CyclicDependenciesReachACompleteFixedPoint) {
  auto system = FBVClock::new_clock_system();
  FBVClock a(system, 0);
  FBVClock b(system, 1);
  FBVClock c(system, 2);
  FBVClock d(system, 3);

  a += b;
  b += a;
  a += c;
  b += d;

  for (int dimension = 0; dimension < 4; ++dimension) {
    EXPECT_TRUE(a[dimension]);
    EXPECT_TRUE(b[dimension]);
  }

  FBVClock::delete_clock_system(system);
}

TEST(FBVClockTest, RejectsClocksFromDifferentSystems) {
  auto destination_system = FBVClock::new_clock_system();
  auto source_system = FBVClock::new_clock_system();
  FBVClock destination(destination_system, 0);
  FBVClock same_id_source(source_system, 1);

  EXPECT_THROW(destination += same_id_source, std::invalid_argument);
  EXPECT_FALSE(destination[1]);

  FBVClock another_source(source_system, 2);
  EXPECT_THROW(destination += another_source, std::invalid_argument);

  FBVClock::delete_clock_system(destination_system);
  FBVClock::delete_clock_system(source_system);
}

TEST(FBVClockTest, StaleHandlesCannotAccessAReusedSystemSlot) {
  auto old_system = FBVClock::new_clock_system();
  FBVClock old(old_system, 0);
  FBVClock::delete_clock_system(old_system);

  auto replacement_system = FBVClock::new_clock_system();
  EXPECT_NE(old_system, replacement_system);
  FBVClock fresh(replacement_system, 1);

  EXPECT_THROW(static_cast<void>(old[1]), std::logic_error);
  EXPECT_THROW(static_cast<void>(old.size()), std::logic_error);
  EXPECT_THROW(static_cast<void>(old.to_string()), std::logic_error);
  EXPECT_THROW(old += fresh, std::logic_error);
  EXPECT_THROW(FBVClock::delete_clock_system(old_system), std::logic_error);

  FBVClock::delete_clock_system(replacement_system);
}

TEST(FBVClockTest, RejectsDuplicateAndNegativeDimensions) {
  auto system = FBVClock::new_clock_system();
  FBVClock owner(system, 0);

  EXPECT_THROW(FBVClock duplicate(system, 0), std::invalid_argument);
  EXPECT_THROW(FBVClock negative(system, -1), std::out_of_range);
  EXPECT_THROW(static_cast<void>(owner[-1]), std::out_of_range);

  FBVClock::ClockSystemID invalid_system;
  EXPECT_THROW(FBVClock invalid(invalid_system, 0), std::logic_error);
  EXPECT_THROW(FBVClock::delete_clock_system(invalid_system), std::logic_error);

  FBVClock::delete_clock_system(system);
}

TEST(FBVClockTest, InvalidatedHandleOperationsFailDeterministically) {
  auto system = FBVClock::new_clock_system();
  FBVClock clock(system, 0);
  clock.invalidate();

  EXPECT_THROW(static_cast<void>(clock[0]), std::logic_error);
  EXPECT_THROW(static_cast<void>(clock.size()), std::logic_error);
  EXPECT_THROW(static_cast<void>(clock.to_string()), std::logic_error);

  FBVClock::delete_clock_system(system);
}

TEST(FBVClockTest, LiveDependencyAndBVClockSnapshotAreExplicitlyDifferent) {
  auto system = FBVClock::new_clock_system();
  FBVClock a(system, 0);
  FBVClock b(system, 1);
  FBVClock c(system, 2);

  a += b;
  BVClock snapshot;
  snapshot = a;
  b += c;

  EXPECT_TRUE(a[2]);
  EXPECT_FALSE(snapshot[2]);

  FBVClock::delete_clock_system(system);
}

TEST(FBVClockTest, IntegralDimensionTypesUseOneUnambiguousAccessor) {
  auto system = FBVClock::new_clock_system();
  FBVClock clock(system, 4);

  unsigned int unsigned_dimension = 4;
  std::uint32_t uint32_dimension = 4;
  std::size_t size_dimension = 4;
  long long_dimension = 4;
  std::uint64_t uint64_dimension = 4;

  EXPECT_TRUE(clock[unsigned_dimension]);
  EXPECT_TRUE(clock[uint32_dimension]);
  EXPECT_TRUE(clock[size_dimension]);
  EXPECT_TRUE(clock[long_dimension]);
  EXPECT_TRUE(clock[uint64_dimension]);
  EXPECT_THROW(static_cast<void>(clock[long{-1}]), std::out_of_range);

  FBVClock::delete_clock_system(system);
}
