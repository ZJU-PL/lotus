#include "CFL/Classical/Solvers/Engines/SubcubicAA/SubcubicAA.h"
#include "CFL/Classical/Solvers/Engines/SubcubicAA/SubcubicAAAdapter.h"

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

namespace aa = engines::subcubic;
using AliasPair = std::pair<aa::Node, aa::Node>;

// Figure 7 of Zhang et al., "Efficient Subcubic Alias Analysis for C",
// OOPSLA 2014. This oracle uses Lotus's general CFL worklist, independently of
// the specialized two-phase state machines and the value-alias construction.
const Grammar &zhengRuginaGrammar() {
  static const Grammar grammar = Grammar::parseFromText(R"(
Start:
  V
Terminal:
  a abar d dbar
Variables:
  M DV V MAM Mq MAs MA AMs AM
Productions:
  M -> DV d;
  DV -> dbar V;
  V -> MAM AMs;
  MAM -> MAs Mq;
  Mq -> <epsilon> | M;
  MAs -> <epsilon> | MAs MA;
  MA -> Mq abar;
  AMs -> <epsilon> | AMs AM;
  AM -> a Mq;
)");
  return grammar;
}

LabeledGraph graphFor(const aa::Problem &problem) {
  LabeledGraph graph;
  for (aa::Node node = 0; node < problem.nodes; ++node)
    graph.addVertex("n" + std::to_string(node));
  for (const auto &[source, target] : problem.assignments) {
    graph.addEdge(source, target, "a");
    graph.addEdge(target, source, "abar");
  }
  for (const auto &[source, target] : problem.dereferences) {
    graph.addEdge(source, target, "d");
    graph.addEdge(target, source, "dbar");
  }
  return graph;
}

struct ExpectedAliases {
  std::size_t nodes = 0;
  std::vector<std::uint8_t> memory;
  std::vector<std::uint8_t> value;
  std::size_t memory_count = 0;
  std::size_t value_count = 0;
};

ExpectedAliases solveWithGrammar(const aa::Problem &problem) {
  LabeledGraph graph = graphFor(problem);
  SolverSession session(graph, zhengRuginaGrammar(), SolverBackend::SparseSet);
  session.solve();
  ExpectedAliases expected;
  expected.nodes = problem.nodes;
  expected.memory.resize(problem.nodes * problem.nodes);
  expected.value.resize(problem.nodes * problem.nodes);
  for (aa::Node source = 0; source < problem.nodes; ++source) {
    for (aa::Node target = 0; target < problem.nodes; ++target) {
      const std::size_t index = source * problem.nodes + target;
      expected.memory[index] = session.contains(source, target, "M");
      expected.value[index] = session.contains(source, target, "V");
      expected.memory_count += expected.memory[index];
      expected.value_count += expected.value[index];
    }
  }
  return expected;
}

void expectMatchesGrammar(const aa::Result &result,
                          const ExpectedAliases &expected) {
  ASSERT_EQ(result.nodeCount(), expected.nodes);
  EXPECT_EQ(result.memoryAliasCount(), expected.memory_count);
  if (result.valueAliasesComputed())
    EXPECT_EQ(result.valueAliasCount(), expected.value_count);
  for (aa::Node source = 0; source < expected.nodes; ++source) {
    for (aa::Node target = 0; target < expected.nodes; ++target) {
      const std::size_t index = source * expected.nodes + target;
      EXPECT_EQ(result.memoryAlias(source, target),
                expected.memory[index] != 0)
          << "M(" << source << ", " << target << ")";
      if (result.valueAliasesComputed()) {
        EXPECT_EQ(result.valueAlias(source, target),
                  expected.value[index] != 0)
            << "V(" << source << ", " << target << ")";
      }
    }
  }
}

void expectAllOptionsMatchGrammar(const aa::Problem &problem) {
  const ExpectedAliases expected = solveWithGrammar(problem);
  for (bool fast_sets : {false, true}) {
    for (bool components : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "fast_sets=" << fast_sets
                   << ", components=" << components);
      aa::Options options;
      options.use_fast_sets = fast_sets;
      options.decompose_components = components;
      const aa::Result result = aa::solve(problem, options);
      ASSERT_TRUE(result.valueAliasesComputed());
      expectMatchesGrammar(result, expected);
    }
  }
}

// Figure 4: a, b, e, *c, *d, &b, c, d, &c, &d, in that order.
aa::Problem figureFour() {
  aa::Problem problem;
  problem.nodes = 10;
  problem.assignments = {{0, 1}, {5, 6}, {3, 4}, {2, 3}};
  problem.dereferences = {{5, 1}, {8, 6}, {6, 3}, {9, 7}, {7, 4}};
  return problem;
}

TEST(SubcubicAATest, ReproducesPaperFigureFour) {
  const aa::Problem problem = figureFour();
  const aa::Result result = aa::solve(problem);
  EXPECT_TRUE(result.memoryAlias(1, 3));  // b and *c.
  EXPECT_TRUE(result.memoryAlias(3, 1));
  EXPECT_TRUE(result.valueAlias(0, 4));   // a and *d.
  EXPECT_TRUE(result.valueAlias(2, 4));   // e and *d.
  EXPECT_FALSE(result.valueAlias(0, 2)); // a and e do not alias.
  expectAllOptionsMatchGrammar(problem);
}

TEST(SubcubicAATest, MemoryAndValueAliasesAreNotTransitive) {
  // q = p; q = r; with dereferences x=*p, y=*q, z=*r. The parents of
  // p/q/r ensure that every assignment target has a real reflexive M-path.
  aa::Problem problem;
  problem.nodes = 9;
  problem.assignments = {{0, 1}, {2, 1}};
  problem.dereferences = {{6, 0}, {7, 1}, {8, 2}, {0, 3}, {1, 4}, {2, 5}};
  const aa::Result result = aa::solve(problem);
  EXPECT_TRUE(result.valueAlias(0, 1));
  EXPECT_TRUE(result.valueAlias(1, 2));
  EXPECT_FALSE(result.valueAlias(0, 2));
  // Lemma 1 / Figures 10 and 11: consecutive M-edges must not propagate.
  EXPECT_TRUE(result.memoryAlias(3, 4));
  EXPECT_TRUE(result.memoryAlias(4, 5));
  EXPECT_FALSE(result.memoryAlias(3, 5));
  EXPECT_TRUE(result.valueAlias(3, 4));
  EXPECT_TRUE(result.valueAlias(4, 5));
  EXPECT_FALSE(result.valueAlias(3, 5));
  expectAllOptionsMatchGrammar(problem);
}

TEST(SubcubicAATest, PreservesAddressIdentityAndValueSymmetry) {
  // p = &q; nodes are &q, q, &p, p. V is nullable at every node, whereas
  // M is reflexive only at a dereference target (Section 2.3).
  aa::Problem problem;
  problem.nodes = 4;
  problem.assignments = {{0, 3}};
  problem.dereferences = {{0, 1}, {2, 3}};
  const aa::Result result = aa::solve(problem);
  EXPECT_TRUE(result.valueAlias(0, 3));
  EXPECT_TRUE(result.valueAlias(3, 0));
  EXPECT_FALSE(result.memoryAlias(0, 0));
  EXPECT_FALSE(result.memoryAlias(2, 2));
  EXPECT_TRUE(result.memoryAlias(1, 1));
  EXPECT_TRUE(result.memoryAlias(3, 3));
  for (aa::Node node = 0; node < problem.nodes; ++node)
    EXPECT_TRUE(result.valueAlias(node, node));
  expectAllOptionsMatchGrammar(problem);
}

TEST(SubcubicAATest, SharedDereferenceParentSeedsAllNullableValuePairs) {
  // A canonical expression has one dereference child. The supported fanout
  // extension must seed dbar epsilon d for all children of a shared parent,
  // including non-reflexive pairs; those pairs also propagate to deeper d's.
  aa::Problem problem;
  problem.nodes = 5;
  problem.dereferences = {{0, 1}, {0, 2}, {1, 3}, {2, 4}};
  const aa::Result result = aa::solve(problem);
  EXPECT_TRUE(result.memoryAlias(1, 2));
  EXPECT_TRUE(result.memoryAlias(2, 1));
  EXPECT_TRUE(result.memoryAlias(3, 4));
  EXPECT_TRUE(result.memoryAlias(4, 3));
  EXPECT_FALSE(result.memoryAlias(0, 0));
  EXPECT_FALSE(result.memoryAlias(1, 3));
  expectAllOptionsMatchGrammar(problem);
}

TEST(SubcubicAATest, AssignmentAndDereferenceCyclesReachAFixedPoint) {
  aa::Problem problem;
  problem.nodes = 4;
  problem.assignments = {{0, 1}, {1, 2}, {2, 0}, {1, 1}};
  problem.dereferences = {{3, 0}, {0, 1}, {1, 2}, {2, 3}, {3, 3}};
  expectAllOptionsMatchGrammar(problem);
}

TEST(SubcubicAATest, EmptyGraphHasEmptyRelationsAndVisitors) {
  const aa::Result result = aa::solve(aa::Problem{});
  EXPECT_EQ(result.nodeCount(), 0u);
  EXPECT_TRUE(result.valueAliasesComputed());
  EXPECT_EQ(result.memoryAliasCount(), 0u);
  EXPECT_EQ(result.valueAliasCount(), 0u);
  std::size_t visits = 0;
  const auto visitor = [&](aa::Node, aa::Node) {
    ++visits;
    return true;
  };
  EXPECT_TRUE(result.visitMemoryAliases(visitor));
  EXPECT_TRUE(result.visitValueAliases(visitor));
  EXPECT_EQ(visits, 0u);
  EXPECT_THROW(result.memoryAlias(0, 0), std::out_of_range);
  EXPECT_THROW(result.valueAlias(0, 0), std::out_of_range);
}

TEST(SubcubicAATest, IsolatedNodesHaveOnlyReflexiveValueAliases) {
  aa::Problem problem;
  problem.nodes = 7;
  const aa::Result result = aa::solve(problem);
  EXPECT_EQ(result.memoryAliasCount(), 0u);
  EXPECT_EQ(result.valueAliasCount(), problem.nodes);
  expectAllOptionsMatchGrammar(problem);
}

TEST(SubcubicAATest, ComponentMappingPreservesNoncontiguousOriginalIds) {
  // Two small components interleaved with isolated nodes. Component-local
  // bit positions must never escape into the result's original node IDs.
  aa::Problem problem;
  problem.nodes = 150;
  problem.assignments = {{0, 64}, {120, 149}};
  problem.dereferences = {{0, 128}, {1, 64}, {120, 130}, {121, 149}};
  const aa::Result result = aa::solve(problem);
  EXPECT_TRUE(result.valueAlias(0, 64));
  EXPECT_TRUE(result.valueAlias(120, 149));
  EXPECT_FALSE(result.valueAlias(64, 149));
  EXPECT_FALSE(result.memoryAlias(128, 130));
  EXPECT_TRUE(result.valueAlias(70, 70));
  EXPECT_EQ(result.statistics().connected_components, 144u);
  EXPECT_EQ(result.statistics().largest_component, 4u);
  aa::Options whole_graph_options;
  whole_graph_options.decompose_components = false;
  const aa::Result whole_graph = aa::solve(problem, whole_graph_options);
  EXPECT_EQ(whole_graph.statistics().connected_components, 1u);
  EXPECT_EQ(whole_graph.statistics().largest_component, problem.nodes);
  expectAllOptionsMatchGrammar(problem);
}

TEST(SubcubicAATest, FastAndScalarModesReportTheSelectedSetOperations) {
  const aa::Problem problem = figureFour();
  const aa::Result fast = aa::solve(problem);
  EXPECT_GT(fast.statistics().difference_words, 0u);
  EXPECT_EQ(fast.statistics().candidate_checks, 0u);
  EXPECT_EQ(fast.statistics().nodes, problem.nodes);
  EXPECT_EQ(fast.statistics().assignment_edges, problem.assignments.size());
  EXPECT_EQ(fast.statistics().dereference_edges, problem.dereferences.size());
  EXPECT_EQ(fast.statistics().memory_aliases, fast.memoryAliasCount());
  EXPECT_EQ(fast.statistics().value_aliases, fast.valueAliasCount());

  aa::Options options;
  options.use_fast_sets = false;
  const aa::Result scalar = aa::solve(problem, options);
  EXPECT_GT(scalar.statistics().candidate_checks, 0u);
  EXPECT_EQ(scalar.statistics().difference_words, 0u);
  EXPECT_EQ(scalar.memoryAliasCount(), fast.memoryAliasCount());
  EXPECT_EQ(scalar.valueAliasCount(), fast.valueAliasCount());
}

TEST(SubcubicAATest, FastSetsHandleWordBoundariesAndPartialLastWords) {
  for (std::size_t nodes : {63u, 64u, 65u, 127u, 128u, 129u}) {
    SCOPED_TRACE(::testing::Message() << "nodes=" << nodes);
    aa::Problem problem;
    problem.nodes = nodes;
    // A d-chain makes this a single component. Assignment summaries and
    // their derived memory aliases cross 64-bit word boundaries.
    for (aa::Node node = 0; node + 1 < nodes; ++node)
      problem.dereferences.emplace_back(node, node + 1);
    const aa::Node middle = nodes / 2;
    problem.assignments = {{0, middle}, {middle, nodes - 1}};
    const aa::Result result = aa::solve(problem);
    EXPECT_TRUE(result.valueAlias(0, nodes - 1));
    EXPECT_TRUE(result.memoryAlias(1, middle + 1));
    expectAllOptionsMatchGrammar(problem);
  }
}

TEST(SubcubicAATest, DuplicateEdgesAndInputOrderDoNotChangeAliases) {
  aa::Problem problem = figureFour();
  const ExpectedAliases expected = solveWithGrammar(problem);
  const auto assignments = problem.assignments;
  const auto dereferences = problem.dereferences;
  problem.assignments.insert(problem.assignments.end(), assignments.begin(),
                             assignments.end());
  problem.dereferences.insert(problem.dereferences.end(), dereferences.begin(),
                              dereferences.end());
  std::mt19937 random(20141024);
  for (unsigned trial = 0; trial < 8; ++trial) {
    SCOPED_TRACE(::testing::Message() << "trial=" << trial);
    std::shuffle(problem.assignments.begin(), problem.assignments.end(), random);
    std::shuffle(problem.dereferences.begin(), problem.dereferences.end(), random);
    aa::Options options;
    options.use_fast_sets = (trial & 1u) != 0;
    options.decompose_components = (trial & 2u) != 0;
    expectMatchesGrammar(aa::solve(problem, options), expected);
  }
}

TEST(SubcubicAATest, RandomGraphsMatchTheIndependentFigureSevenGrammar) {
  std::mt19937 random(2660213);
  std::bernoulli_distribution dereference_edge(0.17);
  std::bernoulli_distribution assignment_edge(0.21);
  for (unsigned trial = 0; trial < 128; ++trial) {
    aa::Problem problem;
    problem.nodes = 1 + random() % 9;
    std::vector<bool> has_incoming_d(problem.nodes, false);
    // Permit self-edges, cycles, and multiple dereference children. The
    // assignment-target invariant is the only restriction in this extension.
    for (aa::Node source = 0; source < problem.nodes; ++source) {
      for (aa::Node target = 0; target < problem.nodes; ++target) {
        if (dereference_edge(random)) {
          problem.dereferences.emplace_back(source, target);
          has_incoming_d[target] = true;
        }
      }
    }
    for (aa::Node source = 0; source < problem.nodes; ++source) {
      for (aa::Node target = 0; target < problem.nodes; ++target) {
        if (has_incoming_d[target] && assignment_edge(random))
          problem.assignments.emplace_back(source, target);
      }
    }
    SCOPED_TRACE(::testing::Message()
                 << "trial=" << trial << ", nodes=" << problem.nodes);
    expectAllOptionsMatchGrammar(problem);
  }
}

TEST(SubcubicAATest, MemoryOnlyModePreservesMemoryAndRejectsValueQueries) {
  const aa::Problem problem = figureFour();
  const ExpectedAliases expected = solveWithGrammar(problem);
  for (bool fast_sets : {false, true}) {
    aa::Options options;
    options.use_fast_sets = fast_sets;
    options.compute_value_aliases = false;
    const aa::Result result = aa::solve(problem, options);
    EXPECT_FALSE(result.valueAliasesComputed());
    expectMatchesGrammar(result, expected);
    EXPECT_THROW(result.valueAlias(0, 0), std::logic_error);
    EXPECT_THROW(result.valueAliasCount(), std::logic_error);
    EXPECT_THROW(result.visitValueAliases([](aa::Node, aa::Node) { return true; }),
                 std::logic_error);
    EXPECT_THROW(result.visitValueSuccessors(0, [](aa::Node) { return true; }),
                 std::logic_error);
  }
  // A later full solve is independent of the preceding memory-only solves.
  const aa::Result full_result = aa::solve(problem);
  EXPECT_TRUE(full_result.valueAliasesComputed());
  expectMatchesGrammar(full_result, expected);
}

TEST(SubcubicAATest, VisitorsEnumerateEachOrderedAliasExactlyOnce) {
  const aa::Result result = aa::solve(figureFour());
  std::set<AliasPair> memory;
  std::set<AliasPair> value;
  EXPECT_TRUE(result.visitMemoryAliases([&](aa::Node source, aa::Node target) {
    EXPECT_TRUE(memory.emplace(source, target).second);
    return true;
  }));
  EXPECT_TRUE(result.visitValueAliases([&](aa::Node source, aa::Node target) {
    EXPECT_TRUE(value.emplace(source, target).second);
    return true;
  }));
  EXPECT_EQ(memory.size(), result.memoryAliasCount());
  EXPECT_EQ(value.size(), result.valueAliasCount());
  for (aa::Node source = 0; source < result.nodeCount(); ++source) {
    std::set<aa::Node> memory_successors;
    std::set<aa::Node> value_successors;
    EXPECT_TRUE(result.visitMemorySuccessors(source, [&](aa::Node target) {
      EXPECT_TRUE(memory_successors.insert(target).second);
      return true;
    }));
    EXPECT_TRUE(result.visitValueSuccessors(source, [&](aa::Node target) {
      EXPECT_TRUE(value_successors.insert(target).second);
      return true;
    }));
    for (aa::Node target = 0; target < result.nodeCount(); ++target) {
      const bool is_memory = result.memoryAlias(source, target);
      const bool is_value = result.valueAlias(source, target);
      EXPECT_EQ(memory.count({source, target}) != 0, is_memory);
      EXPECT_EQ(value.count({source, target}) != 0, is_value);
      EXPECT_EQ(memory_successors.count(target) != 0, is_memory);
      EXPECT_EQ(value_successors.count(target) != 0, is_value);
    }
  }
}

TEST(SubcubicAATest, VisitorsStopImmediatelyWhenTheCallbackCancels) {
  const aa::Result result = aa::solve(figureFour());
  std::size_t visits = 0;
  const auto cancel_pair = [&](aa::Node, aa::Node) {
    ++visits;
    return false;
  };
  EXPECT_FALSE(result.visitMemoryAliases(cancel_pair));
  EXPECT_EQ(visits, 1u);
  visits = 0;
  EXPECT_FALSE(result.visitValueAliases(cancel_pair));
  EXPECT_EQ(visits, 1u);
  const auto cancel_successor = [&](aa::Node) {
    ++visits;
    return false;
  };
  visits = 0;
  EXPECT_FALSE(result.visitMemorySuccessors(1, cancel_successor));
  EXPECT_EQ(visits, 1u);
  visits = 0;
  EXPECT_FALSE(result.visitValueSuccessors(1, cancel_successor));
  EXPECT_EQ(visits, 1u);
  // Address a has no memory successor, so no callback can cancel its visit.
  visits = 0;
  EXPECT_TRUE(result.visitMemorySuccessors(0, cancel_successor));
  EXPECT_EQ(visits, 0u);
  expectMatchesGrammar(result, solveWithGrammar(figureFour()));
}

TEST(SubcubicAATest, RejectsOutOfRangeInputEndpoints) {
  for (const AliasPair edge : {AliasPair{2, 1}, AliasPair{0, 2}}) {
    aa::Problem assignment;
    assignment.nodes = 2;
    assignment.assignments = {edge};
    assignment.dereferences = {{0, 1}};
    EXPECT_THROW(aa::solve(assignment), std::out_of_range);
    aa::Problem dereference;
    dereference.nodes = 2;
    dereference.dereferences = {edge};
    EXPECT_THROW(aa::solve(dereference), std::out_of_range);
  }
}

TEST(SubcubicAATest, RejectsAssignmentTargetsWithoutIncomingDereferences) {
  // Lemmas 5 and 6 depend on a real M(target,target), not an artificial seed.
  aa::Problem problem;
  problem.nodes = 2;
  problem.assignments = {{0, 1}};
  EXPECT_THROW(aa::solve(problem), std::invalid_argument);
  problem.assignments = {{0, 0}};
  problem.dereferences = {{0, 1}};
  EXPECT_THROW(aa::solve(problem), std::invalid_argument);
}

TEST(SubcubicAATest, RejectsOutOfRangeResultQueriesAndSuccessorVisits) {
  const aa::Result result = aa::solve(figureFour());
  const aa::Node invalid = result.nodeCount();
  EXPECT_THROW(result.memoryAlias(invalid, 0), std::out_of_range);
  EXPECT_THROW(result.memoryAlias(0, invalid), std::out_of_range);
  EXPECT_THROW(result.valueAlias(invalid, 0), std::out_of_range);
  EXPECT_THROW(result.valueAlias(0, invalid), std::out_of_range);
  EXPECT_THROW(result.visitMemorySuccessors(invalid,
                                           [](aa::Node) { return true; }),
               std::out_of_range);
  EXPECT_THROW(result.visitValueSuccessors(invalid,
                                          [](aa::Node) { return true; }),
               std::out_of_range);
}

TEST(SubcubicAAAdapterTest, ImportsBidirectedPegWithoutChangingNodeIds) {
  const aa::Problem expected = figureFour();
  LabeledGraph graph = graphFor(expected);
  graph.addVertex("isolated");
  const aa::Problem problem = aa::toSubcubicAliasProblem(graph);
  EXPECT_EQ(problem.nodes, expected.nodes + 1);
  EXPECT_EQ(std::set<AliasPair>(problem.assignments.begin(),
                                problem.assignments.end()),
            std::set<AliasPair>(expected.assignments.begin(),
                                expected.assignments.end()));
  EXPECT_EQ(std::set<AliasPair>(problem.dereferences.begin(),
                                problem.dereferences.end()),
            std::set<AliasPair>(expected.dereferences.begin(),
                                expected.dereferences.end()));
  expectMatchesGrammar(aa::solve(problem), solveWithGrammar(problem));
}

TEST(SubcubicAAAdapterTest, RejectsUnknownLabelsAndUnpairedInverseEdges) {
  for (const std::string label : {"a", "abar", "d", "dbar", "call_1"}) {
    SCOPED_TRACE(label);
    LabeledGraph graph;
    graph.addEdge("n0", "n1", label);
    EXPECT_THROW(aa::toSubcubicAliasProblem(graph), std::invalid_argument);
  }
  LabeledGraph wrong_direction;
  wrong_direction.addEdge("n0", "n1", "a");
  wrong_direction.addEdge("n0", "n1", "abar");
  EXPECT_THROW(aa::toSubcubicAliasProblem(wrong_direction),
               std::invalid_argument);
}

} // namespace
} // namespace lotus::cfl::classical
