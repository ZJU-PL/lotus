#include "CFL/Classical/Solvers/Engines/SparseSubset/SparseSubsetCFL.h"
#include "CFL/Classical/Solvers/Engines/SparseSubset/SparseSubsetTabulation.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

using NamedEdge = std::tuple<std::string, NodeId, NodeId>;
namespace ss = engines::sparse_subset;

std::set<NamedEdge> namedEdges(const Relation &relation,
                               const Grammar &grammar) {
  std::set<NamedEdge> result;
  for (const RelationEdge &edge : relation.edges())
    result.emplace(grammar.symbolName(edge.symbol), edge.source, edge.target);
  return result;
}

std::set<NamedEdge> solveWith(SolverBackend backend, const LabeledGraph &input,
                              const Grammar &grammar) {
  LabeledGraph graph = input;
  SolverSession session(graph, grammar, backend);
  session.solve();
  return namedEdges(session.relation(), grammar);
}

/// S -> A B | S S | <epsilon>; A -> a; B -> b.
Grammar nestedGrammar() {
  return Grammar::parseFromText("Start:\n"
                                "  S\n"
                                "Terminal:\n"
                                "  a b\n"
                                "Variables:\n"
                                "  S A B\n"
                                "Productions:\n"
                                "  S -> A B | S S | <epsilon>;\n"
                                "  A -> a;\n"
                                "  B -> b;\n");
}

LabeledGraph nestedGraph() {
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "b");
  graph.addEdge("n2", "n3", "a");
  graph.addEdge("n3", "n4", "b");
  graph.addVertex("isolated");
  return graph;
}

} // namespace

TEST(SparseSubsetCountersTest, AddsLanesWithoutCrossLaneCarries) {
  ss::BitPlaneCounters counters(70, 7);
  EXPECT_EQ(counters.size(), 70u);
  ASSERT_EQ(counters.planeCount(), 3u);

  // Seed every lane with 7: a zero-valued addend must preserve every lane
  // while exercising the full plane width.
  ss::BitPlaneCounters other(70, 7);
  for (std::size_t lane = 0; lane < counters.size(); ++lane) {
    counters.set(lane, 7);
    EXPECT_EQ(counters.get(lane), 7u);
  }
  counters.add(other);
  for (std::size_t lane = 0; lane < counters.size(); ++lane)
    EXPECT_EQ(counters.get(lane), 7u);

  // A carry out of the low lane must not leak into its neighbour.
  counters.set(0, 1);
  counters.set(1, 0);
  other.set(0, 1);
  other.set(1, 2);
  counters.add(other);
  EXPECT_EQ(counters.get(0), 2u);
  EXPECT_EQ(counters.get(1), 2u);
  EXPECT_EQ(counters.get(2), 7u);
}

TEST(SparseSubsetCountersTest, RejectsOutOfRangeLanesAndOverflow) {
  ss::BitPlaneCounters counters(4, 1);
  EXPECT_THROW(counters.get(4), std::out_of_range);
  EXPECT_THROW(counters.set(4, 1), std::out_of_range);

  ss::BitPlaneCounters other(4, 1);
  for (std::size_t lane = 0; lane < 4; ++lane) {
    counters.set(lane, 1);
    other.set(lane, 1);
  }
  EXPECT_THROW(counters.add(other), std::overflow_error);

  ss::BitPlaneCounters narrow(2, 1);
  EXPECT_THROW(narrow.add(other), std::invalid_argument);
}

TEST(SparseSubsetTableTest, SumsColumnSubsetsInsideEachBlock) {
  ss::SparseSubsetTable table(2, 8, 4, 2);
  EXPECT_EQ(table.rows(), 2u);
  EXPECT_EQ(table.columns(), 8u);
  EXPECT_EQ(table.blockSize(), 4u);
  EXPECT_EQ(table.sparsity(), 2u);

  EXPECT_TRUE(table.set(0, 1));
  EXPECT_TRUE(table.set(0, 5));
  EXPECT_TRUE(table.set(1, 1));
  EXPECT_TRUE(table.set(1, 2));
  EXPECT_FALSE(table.set(0, 1));
  EXPECT_TRUE(table.contains(1, 2));
  EXPECT_FALSE(table.contains(0, 2));

  // Columns 1 and 2 share block 0 (summed as one subset vector), column 5
  // lives in block 1, so the query returns one exact vector per block.
  const auto sums = table.query({1, 2, 5});
  ASSERT_EQ(sums.size(), 2u);
  EXPECT_EQ(sums[0].first, 0u);
  EXPECT_EQ(sums[0].second.get(0), 1u);
  EXPECT_EQ(sums[0].second.get(1), 2u);
  EXPECT_EQ(sums[1].first, 1u);
  EXPECT_EQ(sums[1].second.get(0), 1u);
  EXPECT_EQ(sums[1].second.get(1), 0u);

  EXPECT_TRUE(table.query({}).empty());
  EXPECT_THROW(table.query({2, 1}), std::invalid_argument);
  EXPECT_THROW(table.query({1, 1}), std::invalid_argument);
  EXPECT_THROW(table.set(9, 0), std::out_of_range);
}

TEST(SparseSubsetSetPairTest, MatchesBruteForceDifferenceAndIntersection) {
  std::mt19937 rng(17);
  for (int trial = 0; trial < 60; ++trial) {
    const std::size_t n = 5 + static_cast<std::size_t>(trial % 9);
    ss::TwoSidedSetPair::Options options;
    options.groups = 1 + static_cast<std::size_t>(trial % 3);
    options.promotion_threshold = 1 + static_cast<std::size_t>(trial % 3);
    options.block_size = 1 + static_cast<std::size_t>(trial % 5);
    options.sparsity = 1 + static_cast<std::size_t>(trial % 3);

    ss::TwoSidedSetPair pair(n, options);
    std::vector<std::set<std::size_t>> x_family(n), y_family(n);
    std::uniform_int_distribution<std::size_t> line(0, n - 1);
    std::uniform_int_distribution<std::size_t> element(0, n - 1);

    for (int step = 0; step < 60; ++step) {
      const std::size_t l = line(rng);
      const std::size_t e = element(rng);
      if (rng() % 2) {
        pair.insertX(l, e);
        x_family[l].insert(e);
      } else {
        pair.insertY(l, e);
        y_family[l].insert(e);
      }

      const std::size_t v = line(rng);
      const std::size_t u = line(rng);
      std::vector<std::size_t> expected_report;
      std::size_t expected_intersection = 0;
      for (std::size_t x : x_family[v]) {
        if (y_family[u].count(x))
          ++expected_intersection;
        else
          expected_report.push_back(x);
      }

      std::vector<std::size_t> report = pair.report(v, u);
      std::sort(report.begin(), report.end());
      EXPECT_EQ(report, expected_report)
          << "trial " << trial << " step " << step;
      EXPECT_EQ(pair.count(v, u), expected_intersection)
          << "trial " << trial << " step " << step;
      EXPECT_EQ(pair.subset(v, u),
                expected_intersection == x_family[v].size())
          << "trial " << trial << " step " << step;
    }
  }
}

TEST(SparseSubsetSetPairTest, ReportsDifferenceAndSubsetMembership) {
  ss::TwoSidedSetPair pair(3);
  EXPECT_EQ(pair.universe(), 3u);
  EXPECT_THROW(pair.insertX(3, 0), std::out_of_range);
  EXPECT_THROW(pair.insertY(0, 3), std::out_of_range);
  EXPECT_THROW(pair.report(0, 3), std::out_of_range);
  EXPECT_THROW(ss::TwoSidedSetPair(0), std::invalid_argument);

  pair.insertX(0, 1);
  pair.insertX(0, 2);
  pair.insertY(1, 2);
  EXPECT_EQ(pair.report(0, 1), std::vector<std::size_t>{1});
  EXPECT_EQ(pair.count(0, 1), 1u);
  EXPECT_FALSE(pair.subset(0, 1));

  pair.insertY(1, 1);
  EXPECT_TRUE(pair.report(0, 1).empty());
  EXPECT_EQ(pair.count(0, 1), 2u);
  EXPECT_TRUE(pair.subset(0, 1));
}

TEST(SparseSubsetSolverTest, MatchesSparseBitVectorClosure) {
  const Grammar grammar = nestedGrammar();
  const LabeledGraph graph = nestedGraph();

  const auto expected =
      solveWith(SolverBackend::SparseBitVector, graph, grammar);
  EXPECT_EQ(solveWith(SolverBackend::SparseSet, graph, grammar), expected);
  EXPECT_EQ(solveWith(parseSolverBackend("sparse-subset"), graph, grammar),
            expected);
  EXPECT_NE(expected.count({"S", 0, 4}), 0u);
  // Nullable S makes every isolated vertex reach itself.
  EXPECT_NE(expected.count({"S", 5, 5}), 0u);

  const auto direct = ss::solveAllPairs(graph, grammar);
  ASSERT_NE(direct, nullptr);
  EXPECT_EQ(namedEdges(*direct, grammar), expected);
}

TEST(SparseSubsetSolverTest, ValidatesEpsilonAndGrammar) {
  const LabeledGraph graph = nestedGraph();
  const Grammar grammar = nestedGrammar();

  EXPECT_THROW(ss::solveAllPairs(graph, grammar, 0.0), std::invalid_argument);
  EXPECT_THROW(ss::solveAllPairs(graph, grammar, 1.0), std::invalid_argument);

  LabeledGraph invalid_graph = graph;
  invalid_graph.addEdge("x0", "x1", "not_a_terminal");
  EXPECT_THROW(ss::solveAllPairs(invalid_graph, grammar),
               std::invalid_argument);

  LabeledGraph empty_graph;
  const auto empty = ss::solveAllPairs(empty_graph, grammar);
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(empty->edgeCount(), 0u);
}

TEST(SparseSubsetSolverTest, RecomputesFixedPointAfterUpdates) {
  const Grammar grammar = nestedGrammar();
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "b");

  SolverSession session(graph, grammar, parseSolverBackend("sparse-subset"));
  const ReachabilityStats first = session.solve();
  EXPECT_TRUE(session.contains(0, 2, "S"));
  EXPECT_EQ(session.relation().edgeCount(), first.relation_edges);

  const NodeId n3 = session.addNode("n3");
  const NodeId n4 = session.addNode("n4");
  ASSERT_EQ(n3, 3u);
  ASSERT_EQ(n4, 4u);
  EXPECT_TRUE(session.addTerminalEdge(2, n3, "a"));
  EXPECT_TRUE(session.addTerminalEdge(n3, n4, "b"));

  const ReachabilityStats second = session.solve();
  EXPECT_TRUE(session.contains(0, 4, "S"));
  EXPECT_GT(second.processed_work_items, 0u);

  const std::set<NamedEdge> updated = namedEdges(session.relation(), grammar);
  LabeledGraph extended;
  extended.addEdge("n0", "n1", "a");
  extended.addEdge("n1", "n2", "b");
  extended.addEdge("n2", "n3", "a");
  extended.addEdge("n3", "n4", "b");
  EXPECT_EQ(updated, solveWith(SolverBackend::SparseBitVector, extended,
                              grammar));

  // No new node or fact: the session reuses the previous relation.
  const ReachabilityStats third = session.solve();
  EXPECT_EQ(third.added_edges, 0u);
  EXPECT_EQ(namedEdges(session.relation(), grammar), updated);
}

} // namespace lotus::cfl::classical
