#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/Unary/Adaptive.h"

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::interleaved_dyck::unary {
namespace {

using interleaved_dyck::Edge;
using interleaved_dyck::Graph;
using interleaved_dyck::Label;
using interleaved_dyck::LabelKind;
using interleaved_dyck::Vertex;

Graph bidirectedLinearGraph(const std::vector<std::string> &labels) {
  Graph graph;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const Vertex source = static_cast<Vertex>(i);
    const Vertex target = static_cast<Vertex>(i + 1);
    const Label label = Label::parse(labels[i]);
    graph.addEdge(source, target, label);
    graph.addEdge(target, source, label.complement());
  }
  return graph;
}

bool advance(const Label &label, std::size_t counter_bound, std::size_t &first,
             std::size_t &second) {
  switch (label.kind) {
  case LabelKind::OpenParenthesis:
    if (first == counter_bound) {
      return false;
    }
    ++first;
    return true;
  case LabelKind::CloseParenthesis:
    if (first == 0) {
      return false;
    }
    --first;
    return true;
  case LabelKind::OpenBracket:
    if (second == counter_bound) {
      return false;
    }
    ++second;
    return true;
  case LabelKind::CloseBracket:
    if (second == 0) {
      return false;
    }
    --second;
    return true;
  case LabelKind::Neutral:
    return true;
  }
  return false;
}

bool bruteShallowConnected(const Graph &graph, Vertex source, Vertex target,
                           std::size_t threshold, std::size_t counter_bound) {
  const std::size_t width = counter_bound + 1;
  const auto state = [width](Vertex vertex, std::size_t first,
                             std::size_t second) {
    return (static_cast<std::size_t>(vertex) * width + first) * width + second;
  };
  std::vector<unsigned char> seen(graph.vertices().size() * width * width);
  std::deque<std::array<std::size_t, 3>> worklist;
  seen[state(source, 0, 0)] = true;
  worklist.push_back({static_cast<std::size_t>(source), 0, 0});

  while (!worklist.empty()) {
    const auto current = worklist.front();
    worklist.pop_front();
    if (current[0] == static_cast<std::size_t>(target) && current[1] == 0 &&
        current[2] == 0) {
      return true;
    }
    for (const Edge &edge : graph.edges()) {
      if (edge.source != static_cast<Vertex>(current[0])) {
        continue;
      }
      std::size_t first = current[1];
      std::size_t second = current[2];
      if (!advance(edge.label, counter_bound, first, second) ||
          std::min(first, second) > threshold) {
        continue;
      }
      const std::size_t next = state(edge.target, first, second);
      if (!seen[next]) {
        seen[next] = true;
        worklist.push_back(
            {static_cast<std::size_t>(edge.target), first, second});
      }
    }
  }
  return false;
}

std::vector<std::vector<unsigned char>>
bruteBoundedRelation(const Graph &graph, std::size_t vertex_count,
                     std::size_t counter_bound) {
  const std::size_t width = counter_bound + 1;
  const std::size_t state_count = vertex_count * width * width;
  const auto state = [width](std::size_t vertex, std::size_t first,
                             std::size_t second) {
    return (vertex * width + first) * width + second;
  };

  std::vector<std::vector<Edge>> outgoing(vertex_count);
  for (const Edge &edge : graph.edges()) {
    outgoing.at(static_cast<std::size_t>(edge.source)).push_back(edge);
  }

  std::vector<std::vector<unsigned char>> relation(
      vertex_count, std::vector<unsigned char>(vertex_count, false));
  std::vector<unsigned char> seen(state_count);
  std::deque<std::array<std::size_t, 3>> worklist;
  for (std::size_t source = 0; source < vertex_count; ++source) {
    std::fill(seen.begin(), seen.end(), false);
    worklist.clear();
    seen[state(source, 0, 0)] = true;
    worklist.push_back({source, 0, 0});

    while (!worklist.empty()) {
      const auto current = worklist.front();
      worklist.pop_front();
      for (const Edge &edge : outgoing[current[0]]) {
        std::size_t first = current[1];
        std::size_t second = current[2];
        if (!advance(edge.label, counter_bound, first, second)) {
          continue;
        }
        const std::size_t next =
            state(static_cast<std::size_t>(edge.target), first, second);
        if (!seen[next]) {
          seen[next] = true;
          worklist.push_back(
              {static_cast<std::size_t>(edge.target), first, second});
        }
      }
    }

    for (std::size_t target = 0; target < vertex_count; ++target) {
      relation[source][target] = seen[state(target, 0, 0)];
    }
  }
  return relation;
}

TEST(InterleavedDyckUnaryAdaptiveTest, DistinguishesShallowThresholds) {
  const Graph graph = bidirectedLinearGraph({"+1", "+2", "-1", "-2"});
  const AdaptiveSolver solver;

  EXPECT_FALSE(solver.solveShallow(graph, 0).connected(0, 4));
  EXPECT_TRUE(solver.solveShallow(graph, 1).connected(0, 4));
}

TEST(InterleavedDyckUnaryAdaptiveTest, SwitchesBetweenBothShallowArms) {
  const Graph graph = bidirectedLinearGraph(
      {"+2", "+2", "-2", "+1", "+1", "-1", "+2", "-1", "-2", "-2"});
  const AdaptiveResult result = AdaptiveSolver{}.solveShallow(graph, 1);

  EXPECT_TRUE(result.connected(0, 10));
  EXPECT_EQ(result.stats().vertical_control_states, 22U);
  EXPECT_EQ(result.stats().horizontal_control_states, 22U);
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     PhaseTimingDoesNotChangeThePartition) {
  const Graph graph =
      bidirectedLinearGraph({"+1", "+2", "-1", "-2"});
  AdaptiveOptions plain_options;
  plain_options.sparsify = false;
  AdaptiveOptions timed_options = plain_options;
  timed_options.collect_phase_timing = true;

  const auto plain = AdaptiveSolver{}.solve(graph, plain_options);
  const auto timed = AdaptiveSolver{}.solve(graph, timed_options);

  EXPECT_FALSE(plain.stats().phase_timing.enabled);
  EXPECT_TRUE(timed.stats().phase_timing.enabled);
  EXPECT_EQ(plain.stats().vertical_control_states,
            timed.stats().vertical_control_states);
  EXPECT_EQ(plain.stats().horizontal_control_states,
            timed.stats().horizontal_control_states);
  for (Vertex source : graph.vertices())
    for (Vertex target : graph.vertices())
      EXPECT_EQ(plain.connected(source, target), timed.connected(source, target));
}

TEST(InterleavedDyckUnaryAdaptiveTest, HandlesRepeatedArmAlternation) {
  const Graph graph = bidirectedLinearGraph(
      {"+2", "+2", "-2", "+1", "+1", "-1", "+2", "-2", "+1", "-1", "-1", "-2"});
  const AdaptiveSolver solver;

  EXPECT_FALSE(solver.solveShallow(graph, 0).connected(0, 12));
  EXPECT_TRUE(solver.solveShallow(graph, 1).connected(0, 12));
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     ClosingSelfEdgeTriggersCascadingComponentMerges) {
  Graph graph;
  graph.addEdge(0, 0, Label::closeBracket(0));
  graph.addEdge(0, 0, Label::openBracket(0));
  graph.addEdge(0, 1, Label::closeBracket(0));
  graph.addEdge(1, 0, Label::openBracket(0));
  graph.addEdge(1, 2, Label::closeBracket(0));
  graph.addEdge(2, 1, Label::openBracket(0));

  const AdaptiveResult shallow = AdaptiveSolver{}.solveShallow(graph, 0);
  EXPECT_TRUE(shallow.connected(0, 1));
  EXPECT_TRUE(shallow.connected(1, 2));

  const AdaptiveResult exact = AdaptiveSolver{}.solve(graph);
  EXPECT_TRUE(exact.connected(0, 2));
  EXPECT_EQ(exact.stats().quotient_vertices, 1U);
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     ProjectsTypedDelimiterIdsToUnaryCounters) {
  Graph graph;
  const std::vector<std::string> labels = {"op--7", "ob--9", "cp--2", "cb--4"};
  const std::vector<std::string> reverse = {"cp--11", "cb--3", "op--5",
                                            "ob--8"};
  for (std::size_t i = 0; i < labels.size(); ++i) {
    graph.addEdge(static_cast<Vertex>(i), static_cast<Vertex>(i + 1),
                  Label::parse(labels[i]));
    graph.addEdge(static_cast<Vertex>(i + 1), static_cast<Vertex>(i),
                  Label::parse(reverse[i]));
  }

  AdaptiveOptions direct;
  direct.sparsify = false;
  const AdaptiveResult result = AdaptiveSolver{}.solve(graph, direct);
  EXPECT_TRUE(result.connected(0, 4));
  EXPECT_EQ(result.stats().threshold, 30U);
}

TEST(InterleavedDyckUnaryAdaptiveTest, RejectsNonBidirectedInput) {
  Graph directed;
  directed.addEdge(0, 1, Label::openParenthesis(0));
  EXPECT_THROW(AdaptiveSolver{}.solve(directed), std::invalid_argument);
  EXPECT_THROW(Label::parse("mystery"), std::invalid_argument);
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     ExplicitBidirectingProducesMarkedSoundOverapproximation) {
  Graph directed;
  directed.addEdge(0, 1, Label::openParenthesis(0));
  directed.addEdge(1, 2, Label::closeParenthesis(0));

  AdaptiveOptions options;
  options.input_policy = BidirectedInputPolicy::AddMissingReverseEdges;
  const AdaptiveResult result = AdaptiveSolver{}.solve(directed, options);

  EXPECT_TRUE(result.connected(0, 2));
  EXPECT_FALSE(result.stats().input_was_bidirected);
  EXPECT_TRUE(result.stats().overapproximates_original);
  EXPECT_EQ(result.stats().added_reverse_arcs, 2U);
  EXPECT_EQ(result.stats().input_arcs, 2U);
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     MatchesQuadraticConstructionAcrossExhaustiveEdgeMasks) {
  struct CandidateEdge {
    Vertex source;
    Vertex target;
    const char *label;
  };
  constexpr std::size_t vertex_count = 3;
  constexpr std::size_t counter_bound =
      18 * vertex_count * vertex_count + 6 * vertex_count;
  const std::array<CandidateEdge, 7> candidates = {
      CandidateEdge{0, 1, "+1"},  CandidateEdge{0, 1, "+2"},
      CandidateEdge{1, 2, "-1"},  CandidateEdge{1, 2, "-2"},
      CandidateEdge{0, 2, "eps"}, CandidateEdge{0, 0, "+1"},
      CandidateEdge{2, 2, "+2"}};

  for (unsigned mask = 0; mask < (1U << candidates.size()); ++mask) {
    SCOPED_TRACE(::testing::Message() << "edge mask=" << mask);
    Graph graph;
    for (Vertex vertex = 0; vertex < static_cast<Vertex>(vertex_count);
         ++vertex) {
      graph.addVertex(vertex);
    }
    for (std::size_t edge = 0; edge < candidates.size(); ++edge) {
      if ((mask & (1U << edge)) == 0U) {
        continue;
      }
      const CandidateEdge &candidate = candidates[edge];
      const Label label = Label::parse(candidate.label);
      graph.addEdge(candidate.source, candidate.target, label);
      graph.addEdge(candidate.target, candidate.source, label.complement());
    }

    const auto expected =
        bruteBoundedRelation(graph, vertex_count, counter_bound);
    AdaptiveOptions direct_options;
    direct_options.sparsify = false;
    const AdaptiveResult direct = AdaptiveSolver{}.solve(graph, direct_options);
    const AdaptiveResult sparsified = AdaptiveSolver{}.solve(graph);

    for (Vertex source = 0; source < static_cast<Vertex>(vertex_count);
         ++source) {
      for (Vertex target = 0; target < static_cast<Vertex>(vertex_count);
           ++target) {
        const bool oracle = expected[source][target] != 0;
        EXPECT_EQ(direct.connected(source, target), oracle);
        EXPECT_EQ(sparsified.connected(source, target), oracle);
      }
    }
  }
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     MatchesExplicitSmallConfigurationGraphs) {
  constexpr std::size_t vertex_count = 4;
  constexpr std::size_t counter_bound = 48;
  const std::array<std::string, 5> labels = {"+1", "-1", "+2", "-2", "eps"};
  std::mt19937 generator(0xD1D1U);
  std::uniform_int_distribution<unsigned> vertex(0, vertex_count - 1);
  std::uniform_int_distribution<unsigned> label(0, labels.size() - 1);

  for (unsigned sample = 0; sample < 40; ++sample) {
    SCOPED_TRACE(::testing::Message() << "sample=" << sample);
    Graph graph;
    for (Vertex value = 0; value < static_cast<Vertex>(vertex_count); ++value) {
      graph.addVertex(value);
    }
    for (unsigned edge = 0; edge < 8; ++edge) {
      const Vertex source = vertex(generator);
      const Vertex target = vertex(generator);
      const Label forward = Label::parse(labels[label(generator)]);
      graph.addEdge(source, target, forward);
      graph.addEdge(target, source, forward.complement());
    }

    for (std::size_t threshold = 0; threshold <= 1; ++threshold) {
      const AdaptiveResult result =
          AdaptiveSolver{}.solveShallow(graph, threshold);
      for (Vertex source = 0; source < static_cast<Vertex>(vertex_count);
           ++source) {
        for (Vertex target = 0; target < static_cast<Vertex>(vertex_count);
             ++target) {
          EXPECT_EQ(result.connected(source, target),
                    bruteShallowConnected(graph, source, target, threshold,
                                          counter_bound));
        }
      }
    }

    AdaptiveOptions direct_options;
    direct_options.sparsify = false;
    const AdaptiveResult direct = AdaptiveSolver{}.solve(graph, direct_options);
    const AdaptiveResult sparsified = AdaptiveSolver{}.solve(graph);
    for (Vertex source = 0; source < static_cast<Vertex>(vertex_count);
         ++source) {
      for (Vertex target = 0; target < static_cast<Vertex>(vertex_count);
           ++target) {
        EXPECT_EQ(direct.connected(source, target),
                  sparsified.connected(source, target));
      }
    }
  }
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     DecomposesFullButPreservesShallowThresholds) {
  Graph graph;
  const std::array<Label, 4> labels = {
      Label::openParenthesis(0), Label::openBracket(0),
      Label::closeParenthesis(0), Label::closeBracket(0)};
  for (Vertex offset : {-50, 100})
    for (Vertex i = 0; i < 4; ++i) {
      graph.addEdge(offset + i, offset + i + 1, labels[i]);
      graph.addEdge(offset + i + 1, offset + i, labels[i].complement());
    }
  graph.addVertex(999);
  AdaptiveOptions options;
  options.sparsify = false;
  const auto full = AdaptiveSolver{}.solve(graph, options);
  EXPECT_EQ(full.stats().execution.weak_components, 3u);
  EXPECT_EQ(full.stats().execution.largest_component_vertices, 5u);
  EXPECT_EQ(full.stats().threshold, 30u);
  EXPECT_EQ(full.stats().vertical_control_states, 2u * 5u * 31u);
  EXPECT_TRUE(full.connected(-50, -46));
  EXPECT_TRUE(full.connected(100, 104));
  EXPECT_FALSE(full.connected(-50, 100));
  EXPECT_FALSE(full.connected(999, 100));
  for (std::size_t threshold : {0u, 1u, 2u}) {
    const auto shallow = AdaptiveSolver{}.solveShallow(graph, threshold);
    EXPECT_EQ(shallow.stats().threshold, threshold);
    EXPECT_EQ(shallow.stats().vertical_control_states, 10u * (threshold + 1));
    EXPECT_EQ(shallow.connected(-50, -46), threshold != 0);
    EXPECT_EQ(shallow.connected(100, 104), threshold != 0);
    EXPECT_FALSE(shallow.connected(-50, 100));
  }
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     SkipsTrivialAndSingleCounterStateSpaces) {
  Graph graph;
  for (Vertex v = 0; v < 4096; ++v) {
    graph.addVertex(v);
    if (v % 2 == 0) {
      graph.addEdge(v, v, Label::openParenthesis(0));
      graph.addEdge(v, v, Label::closeParenthesis(0));
      graph.addEdge(v, v, Label::openBracket(0));
      graph.addEdge(v, v, Label::closeBracket(0));
    }
  }
  const auto isolated = AdaptiveSolver{}.solve(graph);
  EXPECT_EQ(isolated.stats().execution.trivial_components, 4096u);
  EXPECT_EQ(isolated.stats().vertical_control_states, 0u);
  EXPECT_EQ(isolated.components().size(), 4096u);
  EXPECT_FALSE(isolated.connected(0, 1));
  EXPECT_LT(isolated.stats().execution.peak_working_bytes, 4096u * 256u);

  const auto first = bidirectedLinearGraph({"+1", "-1"});
  const auto second = bidirectedLinearGraph({"+2", "-2"});
  for (const auto &input : {first, second}) {
    const auto result = AdaptiveSolver{}.solveShallow(input, 0);
    EXPECT_TRUE(result.connected(0, 2));
    EXPECT_EQ(result.stats().execution.single_counter_components, 1u);
    EXPECT_EQ(result.stats().vertical_control_states, 0u);
    EXPECT_EQ(result.stats().horizontal_control_states, 0u);
    EXPECT_EQ(result.stats().single_counter_dyck.states, 3u);
  }
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     StreamsArmsAndCompactsEpsilonComponents) {
  Graph graph;
  constexpr Vertex n = 64;
  for (Vertex v = 0; v < n; ++v) {
    graph.addEdge(v, (v + 1) % n, Label::closeParenthesis(0));
    graph.addEdge((v + 1) % n, v, Label::openParenthesis(0));
    graph.addEdge(v, (v + 3) % n, Label::closeBracket(0));
    graph.addEdge((v + 3) % n, v, Label::openBracket(0));
  }
  const auto result = AdaptiveSolver{}.solve(graph);
  const auto &stats = result.stats();
  const auto states = n * (6 * n + 1);
  ASSERT_EQ(stats.quotient_vertices, static_cast<std::size_t>(n));
  EXPECT_EQ(stats.vertical_control_states, static_cast<std::size_t>(states));
  EXPECT_EQ(stats.vertical_dyck.epsilon_components,
            static_cast<std::size_t>(n));
  EXPECT_EQ(stats.horizontal_dyck.epsilon_components,
            static_cast<std::size_t>(n));
  EXPECT_EQ(stats.vertical_dyck.closing_edges,
            static_cast<std::size_t>(states));
  EXPECT_LE(stats.vertical_dyck.stored_closing_edges,
            static_cast<std::size_t>(n));
  EXPECT_EQ(stats.vertical_dyck.epsilon_edges,
            static_cast<std::size_t>(6 * n * n));
  EXPECT_LT(stats.execution.peak_working_bytes,
            static_cast<std::size_t>(states * 64));
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     ChecksOverflowForMixedShallowConstruction) {
  const auto graph = bidirectedLinearGraph({"+1", "+2"});
  EXPECT_THROW(AdaptiveSolver{}.solveShallow(
                   graph, std::numeric_limits<std::size_t>::max()),
               std::overflow_error);
  Graph empty;
  EXPECT_TRUE(AdaptiveSolver{}.solve(empty).components().empty());
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     GeneratedDyckMatchesIndependentEquivalenceClosure) {
  using interleaved_dyck::BidirectedDyckComponentSolver;
  using interleaved_dyck::LabeledStateEdge;
  using interleaved_dyck::StatePair;
  std::mt19937 random(0x51a7);
  for (unsigned trial = 0; trial < 100; ++trial) {
    const std::size_t n = 1 + random() % 8;
    std::vector<StatePair> epsilon;
    std::vector<LabeledStateEdge> closing;
    std::vector<std::vector<bool>> expected(n, std::vector<bool>(n, false));
    for (std::size_t v = 0; v < n; ++v)
      expected[v][v] = true;
    for (unsigned e = 0; e < 4; ++e) {
      const std::size_t a = random() % n, b = random() % n;
      epsilon.push_back({a, b});
      expected[a][b] = expected[b][a] = true;
    }
    for (unsigned e = 0; e < 16; ++e)
      closing.push_back({random() % n, random() % n, random() % 2});
    bool changed = true;
    while (changed) {
      changed = false;
      for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = 0; b < n; ++b)
          for (std::size_t c = 0; c < n; ++c)
            if (expected[a][b] && expected[b][c] && !expected[a][c]) {
              expected[a][c] = true;
              changed = true;
            }
      for (const auto &a : closing)
        for (const auto &b : closing)
          if (a.label == b.label && expected[a.source][b.source] &&
              !expected[a.target][b.target]) {
            expected[a.target][b.target] = expected[b.target][a.target] = true;
            changed = true;
          }
    }
    unsigned epsilon_calls = 0, closing_calls = 0;
    const auto generated = BidirectedDyckComponentSolver{}.solveGenerated(
        n, 2,
        [&](const auto &visit) {
          ++epsilon_calls;
          for (const auto &e : epsilon)
            visit(e.source, e.target);
        },
        [&](const auto &visit) {
          ++closing_calls;
          for (const auto &e : closing)
            visit(e.source, e.target, e.label);
        },
        closing.size(), true);
    EXPECT_EQ(epsilon_calls, 1u);
    EXPECT_EQ(closing_calls, 1u);
    using Arc = std::tuple<std::size_t, std::size_t, std::size_t>;
    std::set<Arc> projected, summary;
    for (const auto &edge : closing)
      projected.emplace(generated.component[edge.source],
                        generated.component[edge.target], edge.label);
    for (const auto &edge : generated.quotient_closing_edges)
      EXPECT_TRUE(summary.emplace(edge.source, edge.target, edge.label).second);
    EXPECT_EQ(summary, projected);
    const auto explicit_edges =
        BidirectedDyckComponentSolver{}.solve(n, 2, epsilon, closing);
    for (std::size_t a = 0; a < n; ++a) {
      EXPECT_LT(generated.component[a], generated.stats.components);
      for (std::size_t b = 0; b < n; ++b) {
        EXPECT_EQ(generated.component[a] == generated.component[b],
                  expected[a][b]);
        EXPECT_EQ(explicit_edges.component[a] == explicit_edges.component[b],
                  expected[a][b]);
      }
    }
  }
  EXPECT_THROW(BidirectedDyckComponentSolver{}.solve(2, 1, {{0, 2}}, {}),
               std::out_of_range);
  EXPECT_THROW(BidirectedDyckComponentSolver{}.solve(2, 1, {}, {{0, 1, 1}}),
               std::out_of_range);
  EXPECT_THROW(BidirectedDyckComponentSolver{}.solve(2, 1, {}, {{0, 2, 0}}),
               std::out_of_range);
  EXPECT_THROW(BidirectedDyckComponentSolver{}.solve(2, 0, {}, {}),
               std::invalid_argument);
}

TEST(InterleavedDyckUnaryAdaptiveTest,
     SparsificationReturnsFunctionalDenseQuotient) {
  Graph graph;
  constexpr Vertex n = 32;
  for (Vertex source = 0; source < n; ++source)
    for (Vertex target = 0; target < n; ++target)
      for (const auto label :
           {Label::closeParenthesis(0), Label::closeBracket(0)}) {
        graph.addEdge(source, target, label);
        graph.addEdge(target, source, label.complement());
      }
  const auto projection = interleaved_dyck::projectToUnary(graph);
  const auto quotient = interleaved_dyck::sparsifyUnaryGraph(projection.graph);
  EXPECT_EQ(quotient.graph.vertex_count, 1u);
  EXPECT_EQ(quotient.graph.edges.size(), 4u);
  EXPECT_EQ(quotient.original_to_quotient.size(), static_cast<std::size_t>(n));
  for (const auto component : quotient.original_to_quotient)
    EXPECT_EQ(component, 0u);
  EXPECT_EQ(quotient.dyck.closing_edges, static_cast<std::size_t>(2 * n * n));
  EXPECT_LE(quotient.dyck.scanned_closing_edges,
            2 * quotient.dyck.stored_closing_edges);
  std::set<std::tuple<std::size_t, std::size_t, unsigned>> unique;
  for (const auto &edge : quotient.graph.edges) {
    EXPECT_TRUE(unique
                    .emplace(edge.source, edge.target,
                             static_cast<unsigned>(edge.label))
                    .second);
    EXPECT_EQ(edge.source, 0u);
    EXPECT_EQ(edge.target, 0u);
  }
}

} // namespace
} // namespace lotus::cfl::interleaved_dyck::unary
