/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <sparta/ConstantAbstractDomain.h>

using namespace sparta;

TEST(ConstantAbstractDomainTest, pointerConstants) {
  int object = 0;
  using Domain = ConstantAbstractDomain<int *>;

  const Domain value(&object);
  EXPECT_TRUE(value.is_value());
  ASSERT_TRUE(value.get_constant().has_value());
  EXPECT_EQ(*value.get_constant(), &object);

  const Domain top;
  const Domain bottom = Domain::bottom();
  EXPECT_TRUE(top.is_top());
  EXPECT_FALSE(top.get_constant().has_value());
  EXPECT_TRUE(bottom.is_bottom());
  EXPECT_FALSE(bottom.get_constant().has_value());
}
