#include "Mosaic/CHCParser.h"
#include "Mosaic/MosaicFixedpoint.h"

#include <gtest/gtest.h>
#include <z3++.h>

namespace lotus::mosaic {
namespace {

constexpr const char *SIMPLE_CHC = R"(
(set-logic HORN)
(define-sort Word () (_ BitVec 4))
(declare-rel p (Word))
(declare-rel fail ())
(declare-var x Word)
(rule (=> (bvugt x (_ bv0 4)) (p x)))
(rule (=> (and (p x) (bvule x (_ bv0 4))) fail))
(query fail)
)";

TEST(MosaicCHCParserTest, ParsesStandardCHCForZ3AndMosaic) {
  z3::context context;
  z3::fixedpoint fixedpoint(context);
  z3::expr query =
      CHCParser(context).parseString(fixedpoint, SIMPLE_CHC, "simple");

  EXPECT_TRUE(query.is_exists());
  EXPECT_EQ(fixedpoint.query(query), z3::unsat);

  MosaicFixedpoint mosaic(context);
  mosaic.from_solver(fixedpoint);
  EXPECT_EQ(mosaic.query(query), z3::unsat);
}

TEST(MosaicCHCParserTest, RejectsMultipleQueries) {
  constexpr const char *MULTI_QUERY_CHC = R"(
  (set-logic HORN)
  (declare-rel first ())
  (declare-rel second ())
  (rule first)
  (rule second)
  (query first)
  (query second)
  )";

  z3::context context;
  z3::fixedpoint fixedpoint(context);
  EXPECT_THROW(CHCParser(context).parseString(fixedpoint, MULTI_QUERY_CHC,
                                              "multiple-queries"),
               std::runtime_error);
}

} // namespace
} // namespace lotus::mosaic
