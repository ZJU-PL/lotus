#include "Mosaic/Int2BvPreprocessor.h"

#include <gtest/gtest.h>
#include <z3++.h>

namespace lotus::mosaic {
namespace {

void expectEquivalent(const z3::expr &actual, const z3::expr &expected) {
  z3::solver solver(actual.ctx());
  solver.add(actual != expected);
  EXPECT_EQ(solver.check(), z3::unsat)
      << "actual: " << actual << "\nexpected: " << expected;
}

class UnsignedPreprocessorTest : public testing::Test {
protected:
  z3::context context;
  z3::expr x = context.int_const("x");
  z3::expr y = context.int_const("y");
  Int2BvPreprocessor preprocessor{context, 4, false};
};

TEST_F(UnsignedPreprocessorTest, FindsSatOverflowConditionForIncrement) {
  expectEquivalent(preprocessor.create_SAT_out_of_bounds(x < y + 1),
                   x >= 0 && x <= 15 && y == 15);
}

TEST_F(UnsignedPreprocessorTest, RejectsMutuallyExclusiveSatOverflow) {
  expectEquivalent(
      preprocessor.create_SAT_out_of_bounds(x <= y - 1 || x >= y + 1),
      context.bool_val(false));
}

TEST_F(UnsignedPreprocessorTest, FindsNoUnsatOverflowForIncrement) {
  expectEquivalent(preprocessor.create_UNSAT_out_of_bounds(x < y + 1),
                   context.bool_val(false));
}

TEST_F(UnsignedPreprocessorTest, FindsUnsatOverflowAtUnsignedBounds) {
  expectEquivalent(
      preprocessor.create_UNSAT_out_of_bounds(x <= y - 1 || x >= y + 1),
      (x == 0 && y == 0) || (x == 15 && y == 15));
}

} // namespace
} // namespace lotus::mosaic
