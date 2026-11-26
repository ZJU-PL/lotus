#include "CFL/Certificates/LotusAdapter.h"
#include "CFL/Classical/Solvers/SolverSession.h"
#include <gtest/gtest.h>
#include <random>

namespace lotus::cfl::certificates {
namespace {
classical::Grammar grammar() {
  return classical::Grammar::parseFromText(
      "Terminals:\n a b\nVariables:\n S\nStart:\n S\n"
      "Productions:\n S -> a S b;\n S -> <epsilon>;\n");
}

TEST(CFLCertificateAdapter, PositiveNegativeAndEpsilonQueries) {
  const auto gr = grammar();
  classical::LabeledGraph graph;
  graph.addEdge("0", "1", "a");
  graph.addEdge("1", "2", "b");
  for (const auto &endpoints : {std::make_pair(0u, 2u), std::make_pair(0u, 1u),
                               std::make_pair(1u, 1u)}) {
    const auto result = solve(gr, graph, endpoints.first, endpoints.second);
    EXPECT_EQ(result.status == cflcert::frontend::ReachabilityStatus::Reachable,
              endpoints.second != 1u || endpoints.first == 1u);
    EXPECT_TRUE(verify(gr, graph, endpoints.first, endpoints.second, result));
    auto corrupted = result;
    corrupted.status = result.status == cflcert::frontend::ReachabilityStatus::Reachable
        ? cflcert::frontend::ReachabilityStatus::Unreachable
        : cflcert::frontend::ReachabilityStatus::Reachable;
    EXPECT_FALSE(verify(gr, graph, endpoints.first, endpoints.second, corrupted));
  }
}

TEST(CFLCertificateAdapter, PreservesNonterminalSeedsAndRejectsUnknownLabels) {
  const auto gr = grammar();
  classical::LabeledGraph graph;
  graph.addEdge("0", "1", "S");
  graph.addVertex("2");
  const auto result = solve(gr, graph, 0, 1);
  EXPECT_EQ(result.status, cflcert::frontend::ReachabilityStatus::Reachable);
  EXPECT_TRUE(verify(gr, graph, 0, 1, result));
  EXPECT_EQ(solve(gr, graph, 0, 2).status,
            cflcert::frontend::ReachabilityStatus::Unreachable);
  const auto reparsed = grammar();
  EXPECT_TRUE(verify(reparsed, graph, 0, 1, result));
  graph.addEdge("1", "2", "unknown");
  EXPECT_THROW(makeQuery(gr, graph, 0, 2), std::invalid_argument);
}

TEST(CFLCertificateAdapter, MatchesLotusSparseSetOnRandomGraphs) {
  const auto gr = classical::Grammar::parseFromText(
      "Terminals:\n a b\nVariables:\n S A\nStart:\n S\n"
      "Productions:\n S -> a S b | A;\n A -> a | <epsilon>;\n");
  std::mt19937 random(42);
  const char *labels[] = {"a", "b", "A", "S"};
  for (int trial = 0; trial < 12; ++trial) {
    classical::LabeledGraph graph;
    for (int i = 0; i < 4; ++i) graph.addVertex(std::to_string(i));
    for (int i = 0; i < 8; ++i) {
      const auto source = random() % 4;
      const auto target = random() % 4;
      graph.addEdge(source, target, labels[random() % 4]);
    }
    auto copy = graph;
    classical::SolverSession reference(copy, gr, classical::SolverBackend::SparseSet);
    reference.solve();
    for (std::size_t source = 0; source < 4; ++source)
      for (std::size_t target = 0; target < 4; ++target) {
        const auto result = solve(gr, graph, source, target);
        EXPECT_EQ(result.status == cflcert::frontend::ReachabilityStatus::Reachable,
                  reference.relation().contains(gr.startSymbolId(), source, target));
        EXPECT_TRUE(verify(gr, graph, source, target, result));
      }
  }
}

TEST(CFLCertificateAdapter, RejectsOperationalMetadata) {
  const auto gr = classical::Grammar::parseFromText(
      "Start:\nS\nTerminal:\na\nNonterminal:\nS\n"
      "Production:\nS\ta\nS\nInsert:\nS\nFollow:\n\nCount:\nS\n");
  classical::LabeledGraph graph;
  graph.addVertex("only");
  ASSERT_TRUE(gr.usesUnidirectionalMetadata());
  EXPECT_THROW(makeQuery(gr, graph, 0, 0), std::invalid_argument);
}

TEST(CFLCertificateAdapter, RejectsInvalidEndpoints) {
  classical::LabeledGraph graph;
  EXPECT_THROW(makeQuery(grammar(), graph, 0, 0), std::out_of_range);
  graph.addVertex("only");
  EXPECT_THROW(makeQuery(grammar(), graph, 0, 1), std::out_of_range);
}
} // namespace
} // namespace lotus::cfl::certificates
