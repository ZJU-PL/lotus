#include "CFL/InterleavedDyck/StagedBounds/Algorithms.h"
#include "CFL/InterleavedDyck/StagedBounds/Solver.h"

#include <random>
#include <sstream>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::interleaved_dyck::staged_bounds {
namespace {

bool contains(const PairSet &pairs, Vertex source, Vertex target) {
  return pairs.count({source, target}) != 0U;
}

// Independent reference: construct every automaton state copy, as the original
// implementation did, then materialize and project the full CFL relation.
Graph fullTaintProduct(const Graph &graph, std::size_t &states) {
  const auto labels = detail::labelIds(graph, Alphabet::Bracket);
  states = labels.size() + 2;
  std::unordered_map<unsigned, std::size_t> state_of;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    state_of[labels[i]] = i + 1;
  }
  Graph product;
  for (const Edge &edge : graph.edges()) {
    const auto add = [&](std::size_t from, std::size_t to, Label label) {
      product.addEdge(detail::productVertex(edge.source, states, from),
                      detail::productVertex(edge.target, states, to), label);
    };
    if (edge.label.kind == LabelKind::OpenBracket) {
      add(0, state_of.at(edge.label.id), Label::neutral());
      for (std::size_t from = 1; from < states; ++from) {
        add(from, states - 1, Label::neutral());
      }
    } else if (edge.label.kind == LabelKind::CloseBracket) {
      add(state_of.at(edge.label.id), 0, Label::neutral());
      add(states - 1, states - 1, Label::neutral());
    } else {
      for (std::size_t state = 0; state < states; ++state) {
        add(state, state, edge.label);
      }
    }
  }
  return product;
}

PairSet fullTaintRegularization(const Graph &graph) {
  std::size_t states = 0;
  const auto product = fullTaintProduct(graph, states);
  PairSet result;
  for (const Pair &pair :
       Solver{}.projectedReachability(product, Alphabet::Parenthesis)) {
    const auto width = static_cast<Vertex>(states);
    const auto end_state = (pair.target % width + width) % width;
    if (pair.source % width == 0 &&
        (end_state == 0 || end_state == width - 1)) {
      const Pair mapped{pair.source / width, pair.target / width};
      if (mapped.source != mapped.target) {
        result.insert(mapped);
      }
    }
  }
  return result;
}

TEST(InterleavedDyckStagedBoundsGraphTest,
     ParsesArtifactDotLabelsAndDeduplicatesEdges) {
  std::istringstream input("digraph G {\n"
                           "  10 -> 20 [label=\"op--7\"];\n"
                           "  10 -> 20 [label=\"op--7\"];\n"
                           "  20 -> 30 [label=\"normal\"];\n"
                           "}\n");
  const Graph graph = Graph::parseDot(input);

  ASSERT_EQ(graph.edges().size(), 2U);
  EXPECT_EQ(graph.edges()[0].label, Label::openParenthesis(7));
  EXPECT_EQ(graph.edges()[1].label, Label::neutral());
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     SeparatesUnionDyckUnderapproximationFromFullInterleavedDyck) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::openBracket(0));
  graph.addEdge(2, 3, Label::closeParenthesis(0));
  graph.addEdge(3, 4, Label::closeBracket(0));

  const Solver solver;
  EXPECT_TRUE(contains(solver.intersection(graph), 0, 4));
  EXPECT_FALSE(contains(solver.underapproximation(graph), 0, 4));
  EXPECT_TRUE(contains(solver.mutualRefinement(graph), 0, 4));
  EXPECT_TRUE(
      contains(solver.projectedReachability(graph, Alphabet::Parenthesis,
                                            GrammarStrength::Parity),
               0, 4));
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     MutualRefinementRejectsDifferentWitnesses) {
  Graph graph;
  // Parenthesis-valid witness carrying an unmatched bracket.
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::openBracket(0));
  graph.addEdge(2, 3, Label::closeParenthesis(0));
  // Bracket-valid witness carrying an unmatched parenthesis.
  graph.addEdge(0, 4, Label::openBracket(0));
  graph.addEdge(4, 5, Label::openParenthesis(0));
  graph.addEdge(5, 3, Label::closeBracket(0));

  const Solver solver;
  EXPECT_TRUE(contains(solver.intersection(graph), 0, 3));
  EXPECT_FALSE(contains(solver.mutualRefinement(graph), 0, 3));
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     FactorizedTracingPreservesTheFullPipeline) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::openBracket(0));
  graph.addEdge(2, 3, Label::closeBracket(0));
  graph.addEdge(3, 4, Label::closeParenthesis(0));
  graph.addEdge(0, 5, Label::openParenthesis(0));
  graph.addEdge(5, 6, Label::openBracket(0));
  graph.addEdge(6, 3, Label::closeBracket(0));

  const Solver solver;
  const ApproximationResult eager = solver.analyze(graph);
  Options options;
  options.factorized_tracing = true;
  std::vector<Method> stages;
  options.stage_completed = [&](Method stage) { stages.push_back(stage); };
  const ApproximationResult factorized =
      solver.analyze(graph, BenchmarkKind::Taint, options);

  EXPECT_EQ(factorized.regularization, eager.regularization);
  EXPECT_EQ(factorized.intersection, eager.intersection);
  EXPECT_EQ(factorized.underapproximation, eager.underapproximation);
  EXPECT_EQ(factorized.mutual_refinement, eager.mutual_refinement);
  EXPECT_EQ(factorized.stronger_grammar, eager.stronger_grammar);
  EXPECT_EQ(factorized.on_demand, eager.on_demand);
  ASSERT_EQ(stages.size(), 6U);
  EXPECT_EQ(stages.back(), Method::OnDemand);
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     MethodStopsAfterTheSelectedPipelineStage) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::openBracket(0));
  graph.addEdge(2, 3, Label::closeBracket(0));
  graph.addEdge(3, 4, Label::closeParenthesis(0));

  Options options;
  options.method = Method::MutualRefinement;
  std::vector<Method> stages;
  options.stage_completed = [&](Method stage) { stages.push_back(stage); };
  const ApproximationResult result =
      Solver{}.analyze(graph, BenchmarkKind::Taint, options);

  EXPECT_TRUE(contains(result.mutual_refinement, 0, 4));
  EXPECT_EQ(stages, (std::vector<Method>{
                        Method::Regularization, Method::Intersection,
                        Method::Underapproximation, Method::MutualRefinement}));
  EXPECT_TRUE(result.stronger_grammar.empty());
  EXPECT_TRUE(result.on_demand.empty());
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     FullPipelineKeepsAConcreteBalancedPath) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::openBracket(0));
  graph.addEdge(2, 3, Label::closeBracket(0));
  graph.addEdge(3, 4, Label::closeParenthesis(0));

  const ApproximationResult result = Solver{}.analyze(graph);
  EXPECT_TRUE(contains(result.intersection, 0, 4));
  EXPECT_TRUE(contains(result.underapproximation, 0, 4));
  EXPECT_TRUE(contains(result.mutual_refinement, 0, 4));
  EXPECT_TRUE(contains(result.stronger_grammar, 0, 4));
  EXPECT_TRUE(contains(result.on_demand, 0, 4));
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     ValueFlowPipelineAppliesSourceSinkCondition) {
  Graph graph;
  graph.addEdge(0, 1, Label::openBracket(0));
  graph.addEdge(1, 2, Label::neutral());
  graph.addEdge(2, 3, Label::closeBracket(0));

  const ApproximationResult result =
      Solver{}.analyze(graph, BenchmarkKind::ValueFlow);
  EXPECT_TRUE(contains(result.regularization, 0, 3));
  EXPECT_TRUE(contains(result.intersection, 0, 3));
  EXPECT_TRUE(contains(result.underapproximation, 0, 3));
  EXPECT_TRUE(contains(result.mutual_refinement, 0, 3));
  EXPECT_TRUE(contains(result.stronger_grammar, 0, 3));
  EXPECT_TRUE(contains(result.on_demand, 0, 3));
}

TEST(InterleavedDyckStagedBoundsSolverTest, ParityRefinementIsComponentLocal) {
  Graph left;
  left.addEdge(998, 2273, Label::closeParenthesis(27));
  left.addEdge(998, 1713, Label::closeParenthesis(45));
  left.addEdge(991, 1003, Label::closeBracket(3));
  left.addEdge(1001, 991, Label::openBracket(2));
  left.addEdge(2311, 992, Label::openParenthesis(27));
  left.addEdge(992, 991, Label::openParenthesis(45));
  left.addEdge(991, 992, Label::openBracket(3));
  left.addEdge(1003, 1001, Label::closeBracket(3));
  left.addEdge(1001, 1011, Label::closeBracket(2));
  left.addEdge(992, 991, Label::openBracket(3));
  left.addEdge(1011, 998, Label::closeBracket(2));
  left.addEdge(1001, 1011, Label::openParenthesis(45));
  left.addEdge(2273, 1423, Label::openBracket(0));
  left.addEdge(1423, 1423, Label::closeBracket(0));

  Graph right;
  right.addEdge(1, 0, Label::openBracket(1));
  right.addEdge(3, 2, Label::closeBracket(1));

  Graph combined = left;
  for (const Edge &edge : right.edges()) {
    combined.addEdge(edge.source, edge.target, edge.label);
  }

  const Solver solver;
  PairSet separate = solver.mutualRefinement(left, GrammarStrength::Parity, 2,
                                             BenchmarkKind::Taint);
  const PairSet right_result = solver.mutualRefinement(
      right, GrammarStrength::Parity, 2, BenchmarkKind::Taint);
  separate.insert(right_result.begin(), right_result.end());
  const PairSet together = solver.mutualRefinement(
      combined, GrammarStrength::Parity, 2, BenchmarkKind::Taint);

  EXPECT_EQ(together.size(), separate.size());
  for (const Pair &pair : separate) {
    EXPECT_NE(together.count(pair), 0U);
  }
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     FactorizedTracingMatchesBothClientsOnRandomGraphs) {
  std::mt19937 random(0xD1C);
  const Label labels[] = {Label::neutral(),           Label::openParenthesis(0),
                          Label::closeParenthesis(0), Label::openParenthesis(1),
                          Label::closeParenthesis(1), Label::openBracket(0),
                          Label::closeBracket(0),     Label::openBracket(1),
                          Label::closeBracket(1)};
  for (int trial = 0; trial < 24; ++trial) {
    SCOPED_TRACE(trial);
    Graph graph;
    for (int edge = 0; edge < 10; ++edge) {
      graph.addEdge(random() % 5, random() % 5, labels[random() % 9]);
    }
    for (auto client : {BenchmarkKind::Taint, BenchmarkKind::ValueFlow}) {
      Options options;
      const auto eager = Solver{}.analyze(graph, client, options);
      options.factorized_tracing = true;
      const auto factorized = Solver{}.analyze(graph, client, options);
      EXPECT_EQ(factorized.regularization, eager.regularization);
      EXPECT_EQ(factorized.intersection, eager.intersection);
      EXPECT_EQ(factorized.underapproximation, eager.underapproximation);
      EXPECT_EQ(factorized.mutual_refinement, eager.mutual_refinement);
      EXPECT_EQ(factorized.stronger_grammar, eager.stronger_grammar);
      EXPECT_EQ(factorized.on_demand, eager.on_demand);
    }
  }
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     StreamingProjectionMatchesMaterializedMapping) {
  Graph graph;
  for (int vertex = -5; vertex < 70; ++vertex) {
    graph.addEdge(vertex, vertex + 1, Label::neutral());
  }
  graph.addEdge(70, 30, Label::openParenthesis(1));
  graph.addEdge(50, 71, Label::closeParenthesis(1));
  const auto map_pair = [](const Pair &pair) -> std::optional<Pair> {
    if (pair.source % 3 != 0 || pair.target % 5 != 0) {
      return std::nullopt;
    }
    return Pair{pair.source / 2, pair.target / 2};
  };
  for (auto alphabet : {Alphabet::Parenthesis, Alphabet::Bracket}) {
    PairSet expected;
    for (const Pair &pair : Solver{}.projectedReachability(graph, alphabet)) {
      if (const auto mapped = map_pair(pair)) {
        expected.insert(*mapped);
      }
    }
    EXPECT_EQ(detail::runClassicProjectedMapped(graph, alphabet, map_pair),
              expected);
  }
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     FactorizedParityAndOnDemandPreserveAnUncertifiedCrossingPath) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(7));
  graph.addEdge(1, 2, Label::openBracket(9));
  graph.addEdge(2, 3, Label::closeParenthesis(7));
  graph.addEdge(3, 4, Label::closeBracket(9));
  for (unsigned groups = 1; groups <= 4; ++groups) {
    SCOPED_TRACE(groups);
    Options options;
    options.parity_groups = groups;
    const auto eager = Solver{}.analyze(graph, BenchmarkKind::Taint, options);
    // The on-demand stage must actually query this pair: it survives the
    // stronger upper bound but is not certified by the union-Dyck lower bound.
    ASSERT_EQ(eager.underapproximation.count({0, 4}), 0U);
    ASSERT_EQ(eager.stronger_grammar.count({0, 4}), 1U);
    ASSERT_EQ(eager.on_demand.count({0, 4}), 1U);
    options.factorized_tracing = true;
    const auto factorized =
        Solver{}.analyze(graph, BenchmarkKind::Taint, options);
    EXPECT_EQ(factorized.mutual_refinement, eager.mutual_refinement);
    EXPECT_EQ(factorized.stronger_grammar, eager.stronger_grammar);
    EXPECT_EQ(factorized.on_demand, eager.on_demand);
  }
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     TaintProductTrimsUnreachableAndNonAcceptingStateCopies) {
  Graph graph;
  graph.addEdge(0, 1, Label::openBracket(0));
  graph.addEdge(1, 2, Label::neutral());
  graph.addEdge(2, 3, Label::closeBracket(0));
  for (int vertex = 20; vertex < 70; ++vertex) {
    graph.addEdge(vertex, vertex + 1, Label::neutral());
  }
  // Reachable in its typed layer, but cannot reach any accepting state.
  graph.addEdge(500, 20, Label::openBracket(99));
  for (unsigned type = 1; type <= 12; ++type) {
    graph.addEdge(100 + 2 * type, 101 + 2 * type, Label::openBracket(type));
  }
  std::size_t old_states = 0, new_states = 0, accept = 0;
  const auto full = fullTaintProduct(graph, old_states);
  const auto trimmed =
      detail::automatonProduct(graph, BenchmarkKind::Taint, new_states, accept);
  EXPECT_EQ(old_states, new_states);
  EXPECT_EQ(accept, 0U);
  EXPECT_LT(trimmed.edges().size() * 4, full.edges().size());
  const auto result = detail::regularization(graph, BenchmarkKind::Taint);
  EXPECT_EQ(result, fullTaintRegularization(graph));
  EXPECT_EQ(result.count({0, 3}), 1U);
  EXPECT_EQ(result.count({500, 70}), 0U);
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     TaintProductKeepsNestedOpeningsAndUnmatchedClosesInTheSink) {
  Graph graph;
  graph.addEdge(0, 1, Label::openBracket(50));
  graph.addEdge(1, 2, Label::neutral());
  graph.addEdge(2, 3, Label::openBracket(99));
  graph.addEdge(3, 4, Label::closeBracket(1234));
  graph.addEdge(4, 5, Label::neutral());
  graph.addEdge(5, 3, Label::neutral());
  const auto result = detail::regularization(graph, BenchmarkKind::Taint);
  EXPECT_EQ(result, fullTaintRegularization(graph));
  EXPECT_EQ(result.count({0, 5}), 1U);
  EXPECT_EQ(result.count({2, 5}), 0U);
}

TEST(InterleavedDyckStagedBoundsSolverTest,
     TrimmedTaintProductMatchesFullProductOnRandomGraphs) {
  std::mt19937 random(0xA170);
  const Label labels[] = {Label::neutral(),           Label::openParenthesis(1),
                          Label::closeParenthesis(1), Label::openBracket(0),
                          Label::closeBracket(0),     Label::openBracket(4),
                          Label::closeBracket(4),     Label::openBracket(9),
                          Label::closeBracket(9)};
  for (int trial = 0; trial < 100; ++trial) {
    SCOPED_TRACE(trial);
    Graph graph;
    const int offset = trial % 5 == 0 ? -3 : 0;
    for (int edge = 0; edge < 18; ++edge) {
      graph.addEdge(static_cast<int>(random() % 6) + offset,
                    static_cast<int>(random() % 6) + offset,
                    labels[random() % 9]);
    }
    graph.addVertex(100);
    EXPECT_EQ(detail::regularization(graph, BenchmarkKind::Taint),
              fullTaintRegularization(graph));
  }
}

} // namespace
} // namespace lotus::cfl::interleaved_dyck::staged_bounds
