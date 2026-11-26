/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in third-party/SPARTA/LICENSE.
 */

#include <sparta/ConstantAbstractDomain.h>
#include <sparta/Exceptions.h>
#include <sparta/FiniteAbstractDomain.h>
#include <sparta/FlatMap.h>
#include <sparta/FlatSet.h>
#include <sparta/PatriciaTreeCore.h>
#include <sparta/SingletonVector.h>

#include <gtest/gtest.h>
#include <deque>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <memory>
#include <string>
#include <type_traits>

namespace {

constexpr sparta::fad_impl::FixedUint<130> kNegativeTwo(-2);
constexpr sparta::fad_impl::FixedUint<8> kByte254(254);
static_assert(sizeof(sparta::fad_impl::FixedUint<4>) == sizeof(uint8_t));
static_assert(sparta::fad_impl::FixedUint<8>(-2) == kByte254,
              "fixed-width construction must preserve two's complement bits");
static_assert(kNegativeTwo == kNegativeTwo,
              "fixed-width comparison must be constexpr in C++17");

TEST(SpartaMigrationTest, exceptionMetadataAcceptsLvaluesAndPreservesType) {
  sparta::invalid_argument error;
  static_assert(
      std::is_same_v<decltype(error << sparta::error_msg("bad value")),
                     sparta::invalid_argument &>);
  error << sparta::error_msg("bad value") << sparta::argument_name("index");
  try {
    SPARTA_THROW_EXCEPTION(error);
    FAIL() << "expected an invalid_argument";
  } catch (const sparta::invalid_argument &caught) {
    const std::string message(caught.what());
    EXPECT_NE(message.find("bad value"), std::string::npos);
    EXPECT_NE(message.find("index"), std::string::npos);
    EXPECT_NE(message.find("MigrationRegressionTest.cpp"), std::string::npos);
  }
}

TEST(SpartaMigrationTest, runtimeCheckDefersExceptionConstruction) {
  int constructions = 0;
  auto exception = [&]() { ++constructions; return sparta::invalid_argument(); };
  SPARTA_RUNTIME_CHECK(true, exception());
  EXPECT_EQ(constructions, 0);
  EXPECT_THROW(SPARTA_RUNTIME_CHECK(false, exception()), sparta::invalid_argument);
  EXPECT_EQ(constructions, 1);
}

TEST(SpartaMigrationTest, standardExceptionsRetainTypeAndSourceMetadata) {
  const std::logic_error original("original message");
  try {
    SPARTA_THROW_EXCEPTION(original);
    FAIL() << "expected logic_error";
  } catch (const std::logic_error &caught) {
    EXPECT_EQ(std::string(caught.what()), "original message");
    ASSERT_NE(sparta::get_error_info<sparta::throw_file>(caught), nullptr);
    EXPECT_NE(sparta::get_error_info<sparta::throw_file>(caught)->find(
                  "MigrationRegressionTest.cpp"),
              std::string::npos);
    ASSERT_NE(sparta::get_error_info<sparta::throw_line>(caught), nullptr);
    EXPECT_GT(*sparta::get_error_info<sparta::throw_line>(caught), 0);
    ASSERT_NE(sparta::get_error_info<sparta::throw_function>(caught), nullptr);
    EXPECT_NE(sparta::get_error_info<sparta::throw_function>(caught)->find("TestBody"), std::string::npos);
  }
}

TEST(SpartaMigrationTest, typedExceptionMetadataSurvivesCopyAndReplacement) {
  struct CustomTag {};
  using CustomInfo = sparta::error_info<CustomTag, std::vector<int>>;
  struct AnyTag {};
  using AnyInfo = sparta::error_info<AnyTag, std::any>;
  sparta::invalid_argument exception;
  exception << sparta::argument_name("old");
  exception << sparta::argument_name("new") << CustomInfo({1, 2, 3});
  EXPECT_EQ(*sparta::get_error_info<sparta::argument_name>(exception), "new");
  exception << AnyInfo(std::any(std::string("nested payload")));
  const auto copy = exception;
  ASSERT_NE(sparta::get_error_info<AnyInfo>(copy), nullptr);
  EXPECT_EQ(std::any_cast<std::string>(*sparta::get_error_info<AnyInfo>(copy)),
            "nested payload");
  ASSERT_NE(sparta::get_error_info<CustomInfo>(copy), nullptr);
  EXPECT_EQ(*sparta::get_error_info<CustomInfo>(copy),
            (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(sparta::get_error_info<sparta::operation_name>(copy), nullptr);
  try {
    SPARTA_THROW_EXCEPTION(copy);
  } catch (const std::exception &caught) {
    ASSERT_NE(sparta::get_error_info<CustomInfo>(caught), nullptr);
    EXPECT_EQ(*sparta::get_error_info<CustomInfo>(caught),
              (std::vector<int>{1, 2, 3}));
  }
}

TEST(SpartaMigrationTest, signedFixedWidthConstruction) {
  EXPECT_EQ(kNegativeTwo.count(), 129u);
  EXPECT_FALSE(kNegativeTwo.test(0));
  EXPECT_TRUE(kNegativeTwo.test(1));
  EXPECT_TRUE(kNegativeTwo.test(129));
  EXPECT_EQ(kNegativeTwo.leading_zeros(), 0u);
}

TEST(SpartaMigrationTest, fixedWidthSignedMinimumWrapsWithoutOverflow) {
  const auto minimum = std::numeric_limits<long long>::min();
  const sparta::fad_impl::FixedUint<65> bits(minimum);
  EXPECT_TRUE(bits.test(63));
  EXPECT_TRUE(bits.test(64));
  EXPECT_FALSE(bits.test(62));
  EXPECT_EQ(bits.count(), 2u);
  EXPECT_EQ(sparta::fad_impl::FixedUint<8>(minimum).count(), 0u);
  EXPECT_EQ(sparta::fad_impl::FixedUint<130>(minimum).count(), 67u);
}

TEST(SpartaMigrationTest, leafCloneDoesNotRetainUnrelatedBranches) {
  using Value = sparta::pt_core::SimpleValue<std::shared_ptr<std::string>>;
  using Node = sparta::pt_core::PatriciaTreeNode<unsigned, Value>;
  using Leaf = Node::LeafType;
  auto left = Leaf::make(0, std::make_shared<std::string>("left"));
  auto right = Leaf::make(1, std::make_shared<std::string>("right"));
  std::weak_ptr<std::string> weak_right = right->value();
  sparta::intrusive_ptr<Node> tree =
      sparta::pt_core::join_trees<unsigned, Value>(0, left, 1, right);
  auto clone = sparta::pt_core::clone_leaf_pointer_by_key(0u, tree);
  auto missing = sparta::pt_core::clone_leaf_pointer_by_key(2u, tree);
  EXPECT_EQ(missing, nullptr);
  EXPECT_EQ(clone.get(), left.get());
  tree.reset();
  left.reset();
  right.reset();
  EXPECT_TRUE(weak_right.expired());
  ASSERT_NE(clone, nullptr);
  EXPECT_EQ(*clone->value(), "left");
}

TEST(SpartaMigrationTest, flatContainersSupportStorageWithoutReserve) {
  using Set = sparta::FlatSet<unsigned, std::greater<unsigned>,
                              std::equal_to<unsigned>, std::deque<unsigned>>;
  Set set{1, 3};
  set.union_with(Set{2, 3});
  set.intersection_with(Set{2, 3, 4});
  set.difference_with(Set{2});
  ASSERT_EQ(set.size(), 1u);
  EXPECT_TRUE(set.contains(3));

  using Map = sparta::FlatMap<unsigned, unsigned,
                              sparta::pt_core::SimpleValue<unsigned>,
                              std::greater<unsigned>, std::equal_to<unsigned>,
                              std::deque<std::pair<unsigned, unsigned>>>;
  Map map{{1, 2}, {3, 4}};
  map.union_with([](unsigned *a, unsigned b) { *a += b; }, Map{{1, 5}, {2, 3}});
  EXPECT_EQ(map.at(1), 7u);
  EXPECT_EQ(map.at(2), 3u);
  map.difference_with([](unsigned *a, unsigned b) { *a -= b; }, map);
  EXPECT_TRUE(map.empty());
}

TEST(SpartaMigrationTest, defaultConstantRepresentationsAreInitialized) {
  sparta::acd_impl::ConstantAbstractValueRepr<int *> pointer;
  EXPECT_EQ(pointer.constant(), nullptr);
  EXPECT_EQ(pointer.kind(), sparta::AbstractValueKind::Top);
  sparta::ConstantAbstractDomain<int *> top;
  const auto copy = top;
  EXPECT_TRUE(copy.is_top());
  EXPECT_FALSE(copy.get_constant());
}

TEST(SpartaMigrationTest, finiteLatticeAcrossWordBoundary) {
  const sparta::BitVectorLattice<unsigned, 70> lattice(
      {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
       18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
       36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
       54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69},
      {{0, 1},   {1, 2},   {2, 3},   {3, 4},   {4, 5},   {5, 6},   {6, 7},
       {7, 8},   {8, 9},   {9, 10},  {10, 11}, {11, 12}, {12, 13}, {13, 14},
       {14, 15}, {15, 16}, {16, 17}, {17, 18}, {18, 19}, {19, 20}, {20, 21},
       {21, 22}, {22, 23}, {23, 24}, {24, 25}, {25, 26}, {26, 27}, {27, 28},
       {28, 29}, {29, 30}, {30, 31}, {31, 32}, {32, 33}, {33, 34}, {34, 35},
       {35, 36}, {36, 37}, {37, 38}, {38, 39}, {39, 40}, {40, 41}, {41, 42},
       {42, 43}, {43, 44}, {44, 45}, {45, 46}, {46, 47}, {47, 48}, {48, 49},
       {49, 50}, {50, 51}, {51, 52}, {52, 53}, {53, 54}, {54, 55}, {55, 56},
       {56, 57}, {57, 58}, {58, 59}, {59, 60}, {60, 61}, {61, 62}, {62, 63},
       {63, 64}, {64, 65}, {65, 66}, {66, 67}, {67, 68}, {68, 69}});
  for (unsigned i = 0; i < 70; ++i) {
    const auto x = lattice.encode(i);
    EXPECT_EQ(lattice.decode(x), i);
    for (unsigned j = 0; j < 70; ++j) {
      const auto y = lattice.encode(j);
      EXPECT_EQ(lattice.leq(x, y), i <= j);
      EXPECT_EQ(lattice.decode(lattice.join(x, y)), std::max(i, j));
      EXPECT_EQ(lattice.decode(lattice.meet(x, y)), std::min(i, j));
    }
  }
}

inline bool reverse_new_comparators = false;
struct CapturedComparator {
  bool descending = reverse_new_comparators;
  bool operator()(unsigned a, unsigned b) const {
    return descending ? a > b : a < b;
  }
};
TEST(SpartaMigrationTest, flatContainersRetainComparatorState) {
  reverse_new_comparators = false;
  using Map = sparta::FlatMap<unsigned, unsigned,
                              sparta::pt_core::SimpleValue<unsigned>,
                              CapturedComparator>;
  Map map{{1, 10}, {2, 20}, {3, 30}};
  sparta::FlatSet<unsigned, CapturedComparator> set{1, 2, 3};
  reverse_new_comparators = true;
  EXPECT_EQ(map.at(1), 10u);
  EXPECT_TRUE(set.contains(1));
  map.insert_or_assign(2, 21);
  set.insert(2);
  EXPECT_EQ(map.size(), 3u);
  EXPECT_EQ(map.at(2), 21u);
  EXPECT_EQ(set.size(), 3u);
  auto copy = map;
  EXPECT_EQ(copy.at(1), 10u);
  copy.remove(2);
  EXPECT_EQ(copy.size(), 2u);
  reverse_new_comparators = false;
}

struct MagnitudeLess {
  bool operator()(int left, int right) const {
    return std::abs(left) < std::abs(right);
  }
};

TEST(SpartaMigrationTest, orderedContainerLookupUsesComparatorEquivalence) {
  using Map = sparta::FlatMap<int, int, sparta::pt_core::SimpleValue<int>,
                              MagnitudeLess>;
  Map map;
  map.insert_or_assign(1, 2).insert_or_assign(-1, 3);
  ASSERT_EQ(map.size(), 1u);
  EXPECT_EQ(map.at(1), 3);
  EXPECT_EQ(map.at(-1), 3);
  map.update([](int *value) { ++*value; }, -1);
  EXPECT_EQ(map.at(1), 4);
  map.remove(-1);
  EXPECT_TRUE(map.empty());
  sparta::FlatSet<int, MagnitudeLess> set{1, -1, 2, -2};
  EXPECT_EQ(set.size(), 2u);
  EXPECT_TRUE(set.contains(-1));
  set.remove(-1);
  EXPECT_FALSE(set.contains(1));
}

TEST(SpartaMigrationTest, flatSetSelfOperationsAndDescendingDifference) {
  sparta::FlatSet<int> set{1, 2, 3, 4};
  set.intersection_with(set);
  EXPECT_EQ(set.size(), 4u);
  set.union_with(set);
  EXPECT_EQ(set.size(), 4u);
  set.difference_with(set);
  EXPECT_TRUE(set.empty());
  sparta::FlatSet<int, std::greater<int>> descending{1, 2, 3, 4};
  descending.difference_with(decltype(descending){2, 4});
  const std::vector<int> expected{3, 1};
  EXPECT_EQ(std::vector<int>(descending.begin(), descending.end()), expected);
}

TEST(SpartaMigrationTest, compactPointerConstantsPreserveTagsAndNullValues) {
  static_assert(sizeof(sparta::ConstantAbstractDomain<int *>) == sizeof(int *));
  static_assert(sizeof(sparta::intrusive_ptr<sparta::pt_core::PatriciaTreeNode<
                           unsigned, sparta::pt_core::SimpleValue<int>>>) ==
                sizeof(void *));
  using Pointer = sparta::ConstantAbstractDomain<int *>;
  int one = 1, two = 2;
  Pointer value(&one);
  EXPECT_EQ(value.get_constant().value(), &one);
  value.set_to_top();
  EXPECT_FALSE(value.get_constant());
  value.meet_with(Pointer(&two));
  EXPECT_EQ(value.get_constant().value(), &two);
  value.join_with(Pointer(nullptr));
  EXPECT_TRUE(value.is_top());
  value = Pointer(nullptr);
  ASSERT_TRUE(value.get_constant());
  EXPECT_EQ(value.get_constant().value(), nullptr);
  // Unaligned and erased pointer types use the generic representation.
  char letter = 'a';
  EXPECT_EQ(
      sparta::ConstantAbstractDomain<char *>(&letter).get_constant().value(),
      &letter);
  EXPECT_EQ(sparta::ConstantAbstractDomain<void *>(&one).get_constant().value(),
            &one);
}

TEST(SpartaMigrationTest, singletonBucketsStayInlineAndRetainRandomAccess) {
  sparta::SingletonVector<int> sequence;
  sequence.push_back(3);
  EXPECT_TRUE(sequence.uses_inline_storage());
  auto copy = sequence;
  EXPECT_TRUE(copy.uses_inline_storage());
  EXPECT_EQ(*copy.begin(), 3);
  sequence.emplace(sequence.begin(), 1);
  EXPECT_FALSE(sequence.uses_inline_storage());
  sequence.emplace(sequence.begin() + 1, 2);
  EXPECT_EQ(sequence.end() - sequence.begin(), 3);
  const auto &constant = sequence;
  EXPECT_EQ(constant.begin()[2], 3);
  EXPECT_EQ(constant.end() - sequence.begin(), 3);
  sequence.erase(sequence.begin() + 1);
  EXPECT_EQ(sequence[1], 3);
  sequence.erase(sequence.begin() + 1, sequence.end());
  copy = sequence;
  EXPECT_TRUE(copy.uses_inline_storage());
  EXPECT_EQ(copy[0], 1);
  const std::vector<int> more{4, 5, 6};
  sequence.insert(sequence.end(), more.begin(), more.end());
  EXPECT_EQ(sequence[3], 6);
  sequence.clear();
  sequence.emplace(sequence.end(), 7);
  EXPECT_EQ(sequence[0], 7);
}

struct RefCountedBase {
  mutable unsigned references{0};
  unsigned *destructions;
  explicit RefCountedBase(unsigned *count) : destructions(count) {}
  virtual ~RefCountedBase() { ++*destructions; }
  friend void intrusive_ptr_add_ref(const RefCountedBase *pointer) {
    ++pointer->references;
  }
  friend void intrusive_ptr_release(const RefCountedBase *pointer) {
    if (--pointer->references == 0)
      delete pointer;
  }
};
struct RefCountedDerived : RefCountedBase {
  using RefCountedBase::RefCountedBase;
};

TEST(SpartaMigrationTest, intrusiveOwnershipCopiesCastsMovesAndSelfReset) {
  unsigned destructions = 0;
  sparta::intrusive_ptr<RefCountedDerived> derived(
      new RefCountedDerived(&destructions));
  EXPECT_EQ(derived->references, 1u);
  sparta::intrusive_ptr<RefCountedBase> base(std::move(derived));
  EXPECT_FALSE(derived);
  EXPECT_EQ(base->references, 1u);
  auto copy = base;
  auto cast = sparta::static_pointer_cast<RefCountedDerived>(base);
  EXPECT_EQ(base->references, 3u);
  base.reset(base.get());
  EXPECT_EQ(base->references, 3u);
  cast.reset();
  copy.reset();
  EXPECT_EQ(base->references, 1u);
  sparta::intrusive_ptr<const RefCountedBase> constant(std::move(base));
  EXPECT_FALSE(base);
  constant.reset();
  EXPECT_EQ(destructions, 1u);
}

} // namespace
