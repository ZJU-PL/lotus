/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/FiniteAbstractDomain.h>
#include <sparta/HashedAbstractPartition.h>
#include <sparta/ReducedProductAbstractDomain.h>

#include <gtest/gtest.h>
#include <sstream>
#include <tuple>

#include "AbstractDomainPropertyTest.h"

using namespace sparta;

enum ReducedElements0 { ReducedBOT0, ReducedTOP0 };
enum ReducedElements1 { ReducedBOT1, ReducedA, ReducedB, ReducedTOP1 };
enum ReducedElements2 { ReducedBOT2, ReducedC, ReducedD, ReducedE, ReducedF, ReducedTOP2 };

using ReducedLattice0 = BitVectorLattice<ReducedElements0, /* kCardinality */ 2>;
using ReducedLattice1 = BitVectorLattice<ReducedElements1, /* kCardinality */ 4>;
using ReducedLattice2 = BitVectorLattice<ReducedElements2, /* kCardinality */ 6>;

/*
 *         ReducedTOP0
 *          |
 *         ReducedBOT0
 */
static ReducedLattice0 reducedLattice0({ReducedBOT0, ReducedTOP0}, {{ReducedBOT0, ReducedTOP0}});

/*
 *         ReducedTOP1
 *        /    \
 *       ReducedA      ReducedB
 *        \    /
 *         ReducedBOT1
 */
static ReducedLattice1 reducedLattice1({ReducedBOT1, ReducedA, ReducedB, ReducedTOP1},
                  {{ReducedBOT1, ReducedA}, {ReducedBOT1, ReducedB}, {ReducedA, ReducedTOP1}, {ReducedB, ReducedTOP1}});

/*
 *           ReducedTOP2
 *            |
 *            ReducedF
 *           / \
 *          ReducedD   ReducedE
 *           \ /
 *            ReducedC
 *            |
 *           ReducedBOT2
 */
static ReducedLattice2 reducedLattice2({ReducedBOT2, ReducedC, ReducedD, ReducedE, ReducedF, ReducedTOP2},
                  {{ReducedBOT2, ReducedC}, {ReducedC, ReducedD}, {ReducedC, ReducedE}, {ReducedD, ReducedF}, {ReducedE, ReducedF}, {ReducedF, ReducedTOP2}});

using ReducedD0 =
    FiniteAbstractDomain<ReducedElements0, ReducedLattice0, ReducedLattice0::Encoding, &reducedLattice0>;
using ReducedD1 =
    FiniteAbstractDomain<ReducedElements1, ReducedLattice1, ReducedLattice1::Encoding, &reducedLattice1>;
using ReducedD2 =
    FiniteAbstractDomain<ReducedElements2, ReducedLattice2, ReducedLattice2::Encoding, &reducedLattice2>;

class ReducedD0xD1xD2 final
    : public ReducedProductAbstractDomain<ReducedD0xD1xD2, ReducedD0, ReducedD1, ReducedD2> {
 public:
  // Inherit constructors from ReducedProductAbstractDomain.
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  // For testing purposes we assume that ReducedA and ReducedC have disjoint denotations.
  static void reduce_product(std::tuple<ReducedD0, ReducedD1, ReducedD2>& product) {
    if ((std::get<1>(product).element() == ReducedA) &&
        (std::get<2>(product).element() == ReducedC)) {
      std::get<1>(product) = ReducedD1::bottom();
    }
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(ReducedProductAbstractDomain,
                               AbstractDomainPropertyTest,
                               ReducedD0xD1xD2);

template <>
inline std::vector<ReducedD0xD1xD2>
AbstractDomainPropertyTest<ReducedD0xD1xD2>::non_extremal_values() {
  ReducedD0xD1xD2 tad(std::make_tuple(ReducedD0(ReducedTOP0), ReducedD1(ReducedA), ReducedD2(ReducedD)));
  ReducedD0xD1xD2 tbe(std::make_tuple(ReducedD0(ReducedTOP0), ReducedD1(ReducedB), ReducedD2(ReducedE)));
  return {tad, tbe};
}

TEST(ReducedProductAbstractDomainTest, latticeOperations) {
  ReducedD0xD1xD2 top = ReducedD0xD1xD2::top();
  {
    std::ostringstream expected, out;
    expected << "(" << ReducedTOP0 << ", " << ReducedTOP1 << ", " << ReducedTOP2 << ")";
    out << top;
    EXPECT_EQ(expected.str(), out.str());
  }

  ReducedD0xD1xD2 bottom = ReducedD0xD1xD2::bottom();
  {
    std::ostringstream expected, out;
    expected << "(" << ReducedBOT0 << ", " << ReducedBOT1 << ", " << ReducedBOT2 << ")";
    out << bottom;
    EXPECT_EQ(expected.str(), out.str());
  }

  ReducedD0xD1xD2 tad(std::make_tuple(ReducedD0(ReducedTOP0), ReducedD1(ReducedA), ReducedD2(ReducedD)));
  ReducedD0xD1xD2 tbe(std::make_tuple(ReducedD0(ReducedTOP0), ReducedD1(ReducedB), ReducedD2(ReducedE)));
  ReducedD0xD1xD2 join = tad.join(tbe);
  EXPECT_TRUE(join.get<0>().is_top());
  EXPECT_TRUE(join.get<1>().is_top());
  EXPECT_EQ(ReducedF, join.get<2>().element());
  EXPECT_TRUE(join.equals(tad.widening(tbe)));

  ReducedD0xD1xD2 bottom_meet = tad.meet(tbe);
  EXPECT_TRUE(bottom_meet.is_bottom());
  EXPECT_TRUE(bottom_meet.get<0>().is_bottom());
  EXPECT_TRUE(bottom_meet.get<1>().is_bottom());
  EXPECT_TRUE(bottom_meet.get<2>().is_bottom());

  ReducedD0xD1xD2 tte(std::make_tuple(ReducedD0(ReducedTOP0), ReducedD1(ReducedTOP1), ReducedD2(ReducedE)));
  ReducedD0xD1xD2 meet = tad.meet(tte);
  EXPECT_TRUE(meet.get<0>().is_top());
  EXPECT_EQ(ReducedA, meet.get<1>().element());
  EXPECT_EQ(ReducedC, meet.get<2>().element());
  EXPECT_TRUE(meet.equals(tad.narrowing(tte)));

  ReducedD0xD1xD2 bad(std::make_tuple(ReducedD0(ReducedBOT0), ReducedD1(ReducedA), ReducedD2(ReducedD)));
  EXPECT_TRUE(bad.is_bottom());
  EXPECT_TRUE(bad.get<0>().is_bottom());
  EXPECT_TRUE(bad.get<1>().is_bottom());
  EXPECT_TRUE(bad.get<2>().is_bottom());

  ReducedD0xD1xD2 tac_reduced(std::make_tuple(ReducedD0(ReducedBOT0), ReducedD1(ReducedA), ReducedD2(ReducedC)));
  EXPECT_TRUE(tac_reduced.is_bottom());
}

class ReducedD0xPartition final
    : public ReducedProductAbstractDomain<ReducedD0xPartition,
                                          ReducedD0,
                                          HashedAbstractPartition<int, ReducedD1>> {
 public:
  // Inherit constructors from ReducedProductAbstractDomain.
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  static void reduce_product(
      std::tuple<ReducedD0, HashedAbstractPartition<int, ReducedD1>>& product) {}
};

TEST(ReducedProductAbstractDomainTest, normalizedConstruction) {
  ReducedD0xPartition product;
  // Since a partition's default constructor creates a Bottom instance, the
  // product must be set to Bottom as well.
  EXPECT_TRUE(product.is_bottom());
}

TEST(ReducedProductAbstractDomainTest, destructiveOperations) {
  ReducedD0xD1xD2 tad(std::make_tuple(ReducedD0(ReducedTOP0), ReducedD1(ReducedA), ReducedD2(ReducedD)));
  ReducedD0xD1xD2 tbe(std::make_tuple(ReducedD0(ReducedTOP0), ReducedD1(ReducedB), ReducedD2(ReducedE)));
  ReducedD0xD1xD2 ttf(std::make_tuple(ReducedD0(ReducedTOP0), ReducedD1(ReducedTOP1), ReducedD2(ReducedF)));
  ReducedD0xD1xD2 x = tad;
  ReducedD0xD1xD2 tbe1 = tbe;
  x.join_with(tbe);
  EXPECT_TRUE(x.equals(ttf));
  EXPECT_TRUE(tbe.equals(tbe1));
  x = tad;
  x.widen_with(tbe);
  EXPECT_TRUE(x.equals(ttf));
  EXPECT_TRUE(tbe.equals(tbe1));

  x.apply<1>([](ReducedD1* component) { component->set_to_bottom(); });
  EXPECT_TRUE(x.is_bottom());
  x.apply<1>([](ReducedD1* component) { component->set_to_top(); });
  EXPECT_TRUE(x.is_bottom());

  x = tad;
  x.apply<1>([](ReducedD1* component) { component->set_to_top(); });
  x.apply<2>([](ReducedD2* component) { component->set_to_top(); });
  EXPECT_TRUE(x.is_top());

  x = tad;
  x.meet_with(tbe);
  EXPECT_TRUE(x.is_bottom());
  EXPECT_TRUE(tbe.equals(tbe1));
  x = tbe;
  x.meet_with(ttf);
  EXPECT_TRUE(x.equals(tbe));
  x = tbe;
  x.narrow_with(ttf);
  EXPECT_TRUE(x.equals(tbe));
  EXPECT_TRUE(tbe.equals(tbe1));

  x.set_to_top();
  EXPECT_TRUE(x.is_top());
  x.set_to_bottom();
  EXPECT_TRUE(x.is_bottom());
  x.set_to_top();
  EXPECT_TRUE(x.is_top());

  ReducedD0xD1xD2 tae(std::make_tuple(ReducedD0(ReducedTOP0), ReducedD1(ReducedA), ReducedD2(ReducedE)));
  ReducedD0xD1xD2 tac = tad.meet(tae);
  EXPECT_TRUE(tac.get<0>().is_top());
  EXPECT_EQ(ReducedA, tac.get<1>().element());
  EXPECT_EQ(ReducedC, tac.get<2>().element());
  tac.reduce();
  EXPECT_TRUE(tac.is_bottom());
}
