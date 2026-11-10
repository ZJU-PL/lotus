#include "LCLTestSupport.h"

#include <gtest/gtest.h>

namespace lotus::cfl::interleaved_dyck::lcl {
namespace {

#define LOTUS_LCL_TEST(Name, Function)                                          \
  TEST(InterleavedDyckLCL, Name) { EXPECT_NO_THROW(test::Function()); }

LOTUS_LCL_TEST(EmptyAndIsolated, emptyAndIsolated)
LOTUS_LCL_TEST(CrossingAndNesting, crossingAndNesting)
LOTUS_LCL_TEST(MismatchesAndUnderflow, mismatchesAndUnderflow)
LOTUS_LCL_TEST(GrayNodesRejectSpuriousAcceptance, grayNodesRejectSpuriousAcceptance)
LOTUS_LCL_TEST(NeutralClosure, neutralClosure)
LOTUS_LCL_TEST(SparseIdsAndParallelArcs, sparseIdsAndParallelArcs)
LOTUS_LCL_TEST(DistinctProjectionWitnesses, distinctProjectionWitnesses)
LOTUS_LCL_TEST(ExhaustiveUnaryWords, exhaustiveUnaryWords)
LOTUS_LCL_TEST(ExhaustiveTypedWords, exhaustiveTypedWords)
LOTUS_LCL_TEST(LongWords, longWords)
LOTUS_LCL_TEST(RandomDagSoundness, randomDagSoundness)
LOTUS_LCL_TEST(CyclicWitnessSoundness, cyclicWitnessSoundness)
LOTUS_LCL_TEST(OrderIndependence, orderIndependence)
LOTUS_LCL_TEST(KnownOverapproximation, knownOverapproximation)
LOTUS_LCL_TEST(LateGrayPromotion, lateGrayPromotion)
LOTUS_LCL_TEST(ReferenceFixedPoint, referenceFixedPoint)
LOTUS_LCL_TEST(ResourceLimitsAndValidation, resourceLimitsAndValidation)
LOTUS_LCL_TEST(DotAndReuse, dotAndReuse)

#undef LOTUS_LCL_TEST

} // namespace
TEST(InterleavedDyckLCL, ExposesNonAcceptingSummaryPairs) {
  Graph graph;
  graph.addEdge(10, 20, Label::openParenthesis(3));
  const auto result = Solver{}.analyze(graph);
  EXPECT_FALSE(result.mayReach(10, 20));
  EXPECT_TRUE(result.summary_pairs.count({10, 20}) != 0U);
}

} // namespace lotus::cfl::interleaved_dyck::lcl
