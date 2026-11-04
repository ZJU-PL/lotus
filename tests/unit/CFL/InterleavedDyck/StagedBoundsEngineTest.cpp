#include "CFL/InterleavedDyck/StagedBounds/CnfGrammar.h"
#include "CFL/InterleavedDyck/StagedBounds/CnfGraph.h"
#include "CFL/InterleavedDyck/StagedBounds/CnfTypes.h"

#include <random>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <gtest/gtest.h>

namespace lotus::cfl::interleaved_dyck::staged_bounds::mutual_refinement {
namespace {

using UnaryRecord =
    std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher>;
using BinaryRecord = std::unordered_map<
    Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
    EdgeHasher>;

// Independent, deliberately simple fixed point for checking the optimized
// indexed/difference propagation, not another invocation of the same engine.
std::unordered_set<Edge, EdgeHasher>
referenceClosure(const CnfGrammar &grammar,
                 std::unordered_set<Edge, EdgeHasher> edges, int vertices) {
  for (int symbol : grammar.emptyProductions) {
    for (int vertex = 0; vertex < vertices; ++vertex) {
      edges.emplace(vertex, symbol, vertex);
    }
  }
  std::size_t previous;
  do {
    previous = edges.size();
    for (const auto &rule : grammar.unaryProductions) {
      for (int i = 0; i < vertices; ++i) {
        for (int j = 0; j < vertices; ++j) {
          if (edges.count({i, rule.second, j})) {
            edges.emplace(i, rule.first, j);
          }
        }
      }
    }
    for (const auto &rule : grammar.binaryProductions) {
      for (int i = 0; i < vertices; ++i) {
        for (int j = 0; j < vertices; ++j) {
          for (int k = 0; k < vertices; ++k) {
            if (edges.count({i, rule.second.first, k}) &&
                edges.count({k, rule.second.second, j})) {
              edges.emplace(i, rule.first, j);
            }
          }
        }
      }
    }
  } while (edges.size() != previous);
  return edges;
}

TEST(MutualRefinementFactorizedTracingTest,
     ReconstructsEveryBinaryPivotAndUnaryDerivation) {
  constexpr int A = 0;
  constexpr int B = 1;
  constexpr int Dead = 2;
  constexpr int S = 3;
  constexpr int Left = 4;
  constexpr int Right = 5;
  constexpr int Unary = 6;

  CnfGrammar grammar;
  grammar.addTerminal(A);
  grammar.addTerminal(B);
  grammar.addTerminal(Dead);
  grammar.addNonterminal(S);
  grammar.addNonterminal(Left);
  grammar.addNonterminal(Right);
  grammar.addNonterminal(Unary);
  grammar.addStartSymbol(S);
  grammar.addBinaryProduction(S, Left, Right);
  grammar.addUnaryProduction(S, Unary);
  grammar.addUnaryProduction(Left, A);
  grammar.addUnaryProduction(Right, B);
  grammar.addUnaryProduction(Unary, A);
  grammar.initFastIndices();

  const std::unordered_set<Edge, EdgeHasher> edges{
      std::make_tuple(0, A, 1), std::make_tuple(0, A, 2),
      std::make_tuple(1, B, 3), std::make_tuple(2, B, 3),
      std::make_tuple(0, A, 4), std::make_tuple(3, Dead, 4),
  };
  const std::unordered_set<Edge, EdgeHasher> roots{std::make_tuple(0, S, 3),
                                                   std::make_tuple(0, S, 4)};

  CnfGraph eager_graph;
  eager_graph.reinit(5, edges);
  UnaryRecord unary_record;
  BinaryRecord binary_record;
  const auto eager_result =
      eager_graph.runCFLReachability(grammar, unary_record, binary_record);
  ASSERT_NE(eager_result.count(std::make_tuple(0, S, 3)), 0U);
  ASSERT_NE(eager_result.count(std::make_tuple(0, S, 4)), 0U);
  const auto eager_closure =
      eager_graph.getEdgeClosure(grammar, roots, unary_record, binary_record);

  CnfGraph factorized_graph;
  factorized_graph.reinit(5, edges);
  EXPECT_EQ(factorized_graph.runCFLReachability(grammar), eager_result);
  const auto factorized_closure =
      factorized_graph.getFactorizedEdgeClosure(grammar, roots);

  const std::unordered_set<Edge, EdgeHasher> expected{
      std::make_tuple(0, A, 1), std::make_tuple(0, A, 2),
      std::make_tuple(1, B, 3), std::make_tuple(2, B, 3),
      std::make_tuple(0, A, 4),
  };
  EXPECT_EQ(eager_closure, expected);
  EXPECT_EQ(factorized_closure, eager_closure);
}

TEST(MutualRefinementFactorizedTracingTest,
     HandlesRecursiveAndEmptyProductions) {
  constexpr int A = 0;
  constexpr int Dead = 1;
  constexpr int S = 2;

  CnfGrammar grammar;
  grammar.addTerminal(A);
  grammar.addTerminal(Dead);
  grammar.addNonterminal(S);
  grammar.addStartSymbol(S);
  grammar.addEmptyProduction(S);
  grammar.addUnaryProduction(S, A);
  grammar.addBinaryProduction(S, S, S);
  grammar.initFastIndices();

  const std::unordered_set<Edge, EdgeHasher> edges{std::make_tuple(0, A, 1),
                                                   std::make_tuple(1, A, 2),
                                                   std::make_tuple(0, Dead, 2)};
  const std::unordered_set<Edge, EdgeHasher> roots{std::make_tuple(0, S, 2)};

  CnfGraph eager_graph;
  eager_graph.reinit(3, edges);
  UnaryRecord unary_record;
  BinaryRecord binary_record;
  eager_graph.runCFLReachability(grammar, unary_record, binary_record);
  const auto eager_closure =
      eager_graph.getEdgeClosure(grammar, roots, unary_record, binary_record);

  CnfGraph factorized_graph;
  factorized_graph.reinit(3, edges);
  factorized_graph.runCFLReachability(grammar);
  const auto factorized_closure =
      factorized_graph.getFactorizedEdgeClosure(grammar, roots);

  const std::unordered_set<Edge, EdgeHasher> expected{std::make_tuple(0, A, 1),
                                                      std::make_tuple(1, A, 2)};
  EXPECT_EQ(eager_closure, expected);
  EXPECT_EQ(factorized_closure, eager_closure);
}

TEST(MutualRefinementFactorizedTracingTest,
     MatchesEagerForRandomGrammarsAndRootSubsets) {
  std::mt19937 random(0xCF17);
  for (int trial = 0; trial < 100; ++trial) {
    SCOPED_TRACE(trial);
    CnfGrammar grammar;
    for (int terminal = 0; terminal < 3; ++terminal) {
      grammar.addTerminal(terminal);
    }
    for (int symbol = 3; symbol < 7; ++symbol) {
      grammar.addNonterminal(symbol);
      grammar.addUnaryProduction(symbol, random() % 7);
      grammar.addUnaryProduction(symbol, random() % 7);
      grammar.addBinaryProduction(symbol, random() % 7, random() % 7);
      grammar.addBinaryProduction(symbol, random() % 7, random() % 7);
      if (random() % 2 == 0) {
        grammar.addEmptyProduction(symbol);
      }
    }
    grammar.addStartSymbol(3);
    grammar.initFastIndices();
    std::unordered_set<Edge, EdgeHasher> edges;
    for (int i = 0; i < 15; ++i) {
      edges.emplace(random() % 5, random() % 3, random() % 5);
    }
    CnfGraph eager;
    eager.reinit(5, edges);
    UnaryRecord unary;
    BinaryRecord binary;
    const auto roots = eager.runCFLReachability(grammar, unary, binary);
    CnfGraph factorized;
    factorized.reinit(5, edges);
    EXPECT_EQ(factorized.runCFLReachability(grammar), roots);
    const auto reference = referenceClosure(grammar, edges, 5);
    for (int i = 0; i < 5; ++i) {
      for (int j = 0; j < 5; ++j) {
        for (int symbol = 0; symbol < 7; ++symbol) {
          EXPECT_EQ(factorized.hasEdge({i, symbol, j}),
                    reference.count({i, symbol, j}) != 0);
        }
        EXPECT_EQ(roots.count({i, grammar.startSymbol, j}),
                  reference.count({i, grammar.startSymbol, j}));
      }
    }
    EXPECT_EQ(factorized.getFactorizedEdgeClosure(grammar, roots),
              eager.getEdgeClosure(grammar, roots, unary, binary));
    EXPECT_TRUE(factorized.getFactorizedEdgeClosure(grammar, {}).empty());
    // Reuse the same sorted indices with a fresh bitmap for each target.
    for (const auto &root : roots) {
      EXPECT_EQ(factorized.getFactorizedEdgeClosure(grammar, {root}),
                eager.getEdgeClosure(grammar, {root}, unary, binary));
    }
    factorized.reinit(5, edges);
    EXPECT_EQ(factorized.runCFLReachability(grammar), roots);
    EXPECT_EQ(factorized.getFactorizedEdgeClosure(grammar, roots),
              eager.getEdgeClosure(grammar, roots, unary, binary));
  }
}

TEST(MutualRefinementFactorizedTracingTest,
     HandlesSparseSymbolsAndDuplicateEdgesAcrossSaturationRuns) {
  constexpr int Terminal = -17;
  constexpr int S = 1000000;
  CnfGrammar grammar;
  grammar.addTerminal(Terminal);
  grammar.addNonterminal(S);
  grammar.addStartSymbol(S);
  grammar.addEmptyProduction(S);
  grammar.addUnaryProduction(S, Terminal);
  grammar.addBinaryProduction(S, S, S);
  grammar.initFastIndices();
  const std::unordered_set<Edge, EdgeHasher> edges{
      {0, Terminal, 1}, {1, Terminal, 2}, {2, Terminal, 0}};
  CnfGraph graph;
  graph.reinit(10, edges);
  graph.addEdge({0, Terminal, 1});
  graph.runCFLReachability(grammar);
  EXPECT_EQ(graph.getFactorizedEdgeClosure(grammar, {{0, S, 2}}), edges);
  // Adding an edge after in-place sorting must leave saturation usable.
  graph.addEdge({2, Terminal, 3});
  graph.runCFLReachability(grammar);
  auto expected = edges;
  expected.emplace(2, Terminal, 3);
  EXPECT_EQ(graph.getFactorizedEdgeClosure(grammar, {{0, S, 3}}), expected);
}

TEST(MutualRefinementFactorizedTracingTest,
     PromotesRowsAcrossWordBoundariesAndKeepsAllContributingEdges) {
  CnfGrammar grammar;
  grammar.addTerminal(0);
  grammar.addNonterminal(1);
  grammar.addStartSymbol(1);
  grammar.addEmptyProduction(1);
  grammar.addUnaryProduction(1, 0);
  grammar.addBinaryProduction(1, 1, 1);
  grammar.initFastIndices();
  // Dense rows for 70 vertices, compressed rows for sparse vertex IDs in a
  // larger universe. The same graph must have the same non-reflexive closure.
  for (int stride : {1, 101}) {
    SCOPED_TRACE(stride);
    std::unordered_set<Edge, EdgeHasher> edges;
    for (int i = 0; i < 69; ++i) {
      edges.emplace(i * stride, 0, (i + 1) * stride);
    }
    CnfGraph graph;
    graph.reinit(70 * stride, edges);
    const auto roots = graph.runCFLReachability(grammar);
    EXPECT_EQ(roots.size(), 70U * stride + 70U * 69U / 2);
    for (int i = 0; i < 70; ++i) {
      for (int j = 0; j < 70; ++j) {
        EXPECT_EQ(graph.hasEdge({i * stride, 1, j * stride}), i <= j);
      }
    }
    EXPECT_EQ(graph.getFactorizedEdgeClosure(grammar, {{0, 1, 69 * stride}}),
              edges);
    EXPECT_TRUE(
        graph.getFactorizedEdgeClosure(grammar, {{69 * stride, 1, 0}}).empty());
    // A second saturation must report already-present start facts as well.
    EXPECT_EQ(graph.runCFLReachability(grammar), roots);
  }
}

} // namespace
} // namespace lotus::cfl::interleaved_dyck::staged_bounds::mutual_refinement
