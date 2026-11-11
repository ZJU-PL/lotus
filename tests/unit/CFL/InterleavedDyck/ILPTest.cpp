#include "CFL/InterleavedDyck/ILP/Solver.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace lotus::cfl::interleaved_dyck::ilp {
namespace {

TEST(InterleavedDyckILP, RejectsDifferentPathsForDifferentDyckLanguages) {
  // Figure 2 of the POPL 2023 paper: the upper route is balanced only after
  // ignoring brackets, while the lower cyclic route is balanced only after
  // ignoring parentheses. No single route is InterDyck-balanced.
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(1));
  graph.addEdge(1, 2, Label::openBracket(2));
  graph.addEdge(2, 3, Label::closeParenthesis(1));
  graph.addEdge(3, 4, Label::closeBracket(1));
  graph.addEdge(0, 5, Label::openParenthesis(2));
  graph.addEdge(5, 6, Label::openBracket(2));
  graph.addEdge(6, 5, Label::closeParenthesis(1));
  graph.addEdge(5, 4, Label::closeBracket(2));

  const auto result = Solver{}.analyze(graph, 0, 4);
  EXPECT_FALSE(result.may_reach);
}

TEST(InterleavedDyckILP, AcceptsBalancedPathAndEmptyPath) {
  Graph graph;
  graph.addEdge(10, 20, Label::openParenthesis(7));
  graph.addEdge(20, 30, Label::openBracket(3));
  graph.addEdge(30, 40, Label::closeParenthesis(7));
  graph.addEdge(40, 50, Label::closeBracket(3));
  const Solver solver;
  EXPECT_TRUE(solver.analyze(graph, 10, 50).may_reach);
  EXPECT_TRUE(solver.analyze(graph, 20, 20).may_reach);
}

TEST(InterleavedDyckILP, RejectsUnbalancedAcyclicPath) {
  Graph graph;
  graph.addEdge(1, 2, Label::openParenthesis(0));
  EXPECT_FALSE(Solver{}.analyze(graph, 1, 2).may_reach);
}

TEST(InterleavedDyckILP, OptionalUnmatchedOpenEndpoint) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(9));
  EXPECT_FALSE(Solver{}.analyze(graph, 0, 1).may_reach);
  Options options;
  options.allow_unmatched_opens = true;
  EXPECT_TRUE(Solver{}.analyze(graph, 0, 1, options).may_reach);
}

TEST(InterleavedDyckILP, NeutralEdgesAndMissingVertices) {
  Graph graph;
  graph.addEdge(1, 2, Label::neutral());
  EXPECT_TRUE(Solver{}.analyze(graph, 1, 2).may_reach);
  EXPECT_FALSE(Solver{}.analyze(graph, 1, 99).may_reach);
}

TEST(InterleavedDyckILP, AcceptsAPathThroughARepeatedBalancedCycle) {
  Graph graph;
  // 0 -> 1 is a balanced cycle; the final route 0 -> 2 -> 3 is balanced.
  graph.addEdge(0, 1, Label::openParenthesis(4));
  graph.addEdge(1, 0, Label::closeParenthesis(4));
  graph.addEdge(0, 2, Label::openParenthesis(4));
  graph.addEdge(2, 3, Label::closeParenthesis(4));
  const auto result = Solver{}.analyze(graph, 0, 3);
  EXPECT_TRUE(result.may_reach);
  EXPECT_GT(result.statistics.integer_variables, 0U);
}

TEST(InterleavedDyckILP, SolverCanChoosePositiveCycleTraversalCount) {
  Graph graph;
  graph.addEdge(0, 0, Label::openParenthesis(5));
  graph.addEdge(0, 1, Label::closeParenthesis(5));
  const auto result = Solver{}.analyze(graph, 0, 1);
  EXPECT_TRUE(result.may_reach);
  EXPECT_GT(result.statistics.integer_variables, 0U);
}

TEST(InterleavedDyckILP, OneValidAlternativeIsEnough) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0)); // invalid alternative
  graph.addEdge(1, 3, Label::neutral());
  graph.addEdge(0, 2, Label::openParenthesis(0));
  graph.addEdge(2, 3, Label::closeParenthesis(0)); // valid alternative
  EXPECT_TRUE(Solver{}.analyze(graph, 0, 3).may_reach);
}

TEST(InterleavedDyckILP, LclRefinementRejectsAParenthesisOrderingViolation) {
  // Example 4.5 in the POPL 2023 paper. Parenthesis counts balance, but
  // M1 appears before M2 and therefore violates the stack order for L1 L2.
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(1));
  graph.addEdge(1, 2, Label::openBracket(1));
  graph.addEdge(2, 3, Label::openParenthesis(2));
  graph.addEdge(3, 4, Label::closeParenthesis(1));
  graph.addEdge(4, 5, Label::closeParenthesis(2));
  graph.addEdge(5, 6, Label::closeBracket(1));
  EXPECT_FALSE(Solver{}.analyze(graph, 0, 6).may_reach);
  Options options;
  options.enable_lcl_refinement = false;
  EXPECT_TRUE(Solver{}.analyze(graph, 0, 6, options).may_reach);
}

TEST(InterleavedDyckILP, DistinctParenthesisKindsDoNotCancel) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::closeParenthesis(1));
  EXPECT_FALSE(Solver{}.analyze(graph, 0, 2).may_reach);
}

TEST(InterleavedDyckILP, LclRefinementCanBeDisabled) {
  Graph graph;
  graph.addEdge(1, 2, Label::openParenthesis(0));
  Options options;
  options.enable_lcl_refinement = false;
  EXPECT_FALSE(Solver{}.analyze(graph, 1, 2, options).may_reach);
}

TEST(InterleavedDyckILP, ResourceLimitsAreReported) {
  Graph graph;
  graph.addEdge(1, 2, Label::openParenthesis(0));
  graph.addEdge(2, 3, Label::closeParenthesis(0));
  Options options;
  options.max_expression_nodes = 2;
  EXPECT_THROW(Solver{}.analyze(graph, 1, 3, options), std::length_error);
}

} // namespace
TEST(InterleavedDyckILP, NormalizesChoiceInsideKleeneStar) {
  Graph graph;
  graph.addEdge(0, 0, Label::openParenthesis(0));
  graph.addEdge(0, 0, Label::closeParenthesis(0));
  graph.addEdge(0, 1, Label::closeParenthesis(0));
  // The path expression contains (open | close)* close. The standard-form
  // rewrite (a | b)* = (a* b*)* must retain a feasible traversal count.
  EXPECT_TRUE(Solver{}.analyze(graph, 0, 1).may_reach);
}

TEST(InterleavedDyckILP, StandardFormExpansionHonorsDisjunctLimit) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 3, Label::closeParenthesis(0));
  graph.addEdge(0, 2, Label::openParenthesis(1));
  graph.addEdge(2, 3, Label::closeParenthesis(1));
  Options options;
  options.max_disjuncts = 1;
  EXPECT_THROW(Solver{}.analyze(graph, 0, 3, options), std::length_error);
}

TEST(InterleavedDyckILP, BackendAvailabilityMatchesBuildConfiguration) {
  EXPECT_TRUE(Options{}.backend == Backend::Z3);
  EXPECT_TRUE(backendAvailable(Backend::Z3));
#ifdef LOTUS_HAVE_GUROBI
  EXPECT_TRUE(backendAvailable(Backend::Gurobi));
#else
  EXPECT_FALSE(backendAvailable(Backend::Gurobi));
#endif
}

#ifndef LOTUS_HAVE_GUROBI
TEST(InterleavedDyckILP, UnavailableGurobiBackendIsRejected) {
  Graph graph;
  graph.addEdge(1, 2, Label::neutral());
  Options options;
  options.backend = Backend::Gurobi;
  EXPECT_THROW(Solver{}.analyze(graph, 1, 2, options), std::invalid_argument);
}
#endif

} // namespace lotus::cfl::interleaved_dyck::ilp
