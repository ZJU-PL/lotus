/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Because of the rules of argument-dependent lookup, we need to include the
// definition of operator<< for ConstantAbstractDomain before that of operator<<
// for DisjointUnionAbstractDomain.
#include <sparta/ConstantAbstractDomain.h>

#include <sparta/DisjointUnionAbstractDomain.h>

#include <gtest/gtest.h>
#include <string>

#include "AbstractDomainPropertyTest.h"

using namespace sparta;

using IntDomain = ConstantAbstractDomain<int>;
using StringDomain = ConstantAbstractDomain<std::string>;
using IntStringDomain = DisjointUnionAbstractDomain<IntDomain, StringDomain>;

INSTANTIATE_TYPED_TEST_SUITE_P(DisjointUnionAbstractDomain,
                               AbstractDomainPropertyTest,
                               IntStringDomain);

template <>
std::vector<IntStringDomain>
AbstractDomainPropertyTest<IntStringDomain>::top_values() {
  return {IntStringDomain{IntDomain::top()},
          IntStringDomain{StringDomain::top()}};
}

template <>
std::vector<IntStringDomain>
AbstractDomainPropertyTest<IntStringDomain>::bottom_values() {
  return {IntStringDomain{IntDomain::bottom()},
          IntStringDomain{StringDomain::bottom()}};
}

template <>
std::vector<IntStringDomain>
AbstractDomainPropertyTest<IntStringDomain>::non_extremal_values() {
  return {IntStringDomain{IntDomain(0)}, IntStringDomain{StringDomain("foo")}};
}

TEST(DisjointUnionAbstractDomainTest, basicOperations) {
  auto zero = IntStringDomain{IntDomain(0)};
  auto str = IntStringDomain{StringDomain("")};
  EXPECT_TRUE(zero.join(str).is_top());
  EXPECT_TRUE(zero.meet(str).is_bottom());
  EXPECT_NLEQ(zero, str);
  EXPECT_NLEQ(str, zero);
  EXPECT_NE(zero, str);
}

TEST(DisjointUnionAbstractDomainTest, standardVariantAccessAndVisitors) {
  IntStringDomain zero{IntDomain(0)};
  const IntStringDomain str{StringDomain("value")};
  EXPECT_EQ(zero.which(), 0);
  EXPECT_EQ(str.which(), 1);
  EXPECT_EQ(zero.get<IntDomain>().get_constant(), 0);
  EXPECT_FALSE(zero.maybe_get<StringDomain>());
  EXPECT_TRUE(IntStringDomain::top().maybe_get<StringDomain>()->is_top());
  EXPECT_THROW(zero.get<StringDomain>(), std::bad_variant_access);
  zero.apply<IntDomain>([](IntDomain* domain) { domain->set_to_bottom(); });
  EXPECT_TRUE(zero.is_bottom());
  EXPECT_FALSE(zero.which());
  EXPECT_TRUE(IntStringDomain::apply_visitor(
      [](const auto& domain) { return domain.is_bottom(); }, zero));
  EXPECT_TRUE(IntStringDomain::apply_visitor(
      [](const auto& left, const auto& right) {
        return left.is_bottom() && !right.is_bottom();
      }, zero, str));
}

TEST(DisjointUnionAbstractDomainTest, visitorHonorsDeclaredResultType) {
  struct Visitor {
    using result_type = bool;
    int operator()(const IntDomain&) const { return 1; }
    bool operator()(const StringDomain&) const { return false; }
  };
  const IntStringDomain number{IntDomain(7)};
  const IntStringDomain text{StringDomain("x")};
  EXPECT_TRUE(IntStringDomain::apply_visitor(Visitor{}, number));
  EXPECT_FALSE(IntStringDomain::apply_visitor(Visitor{}, text));
}
