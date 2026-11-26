#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/MCFL/Grammar.h"
#include "CFL/InterleavedDyck/MCFL/Graph.h"
#include "CFL/InterleavedDyck/MCFL/InterleavedDyck.h"
#include "CFL/InterleavedDyck/MCFL/Solver.h"

#include <algorithm>
#include <array>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::interleaved_dyck::mcfl {
namespace {

Graph linearGraph(const std::vector<std::string> &labels) {
  Graph graph;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    graph.addEdge(static_cast<Vertex>(i), static_cast<Vertex>(i + 1),
                  labels[i]);
  }
  return graph;
}

std::vector<std::string> labels(const std::vector<Edge> &edges) {
  std::vector<std::string> result;
  for (const Edge &edge : edges) {
    if (!edge.label.empty()) {
      result.push_back(edge.label);
    }
  }
  return result;
}

Grammar copyLanguageGrammar() {
  Grammar grammar;
  const auto start = grammar.addNonterminal("S", 1);
  const auto equal = grammar.addNonterminal("A", 2);
  const auto empty = grammar.addNonterminal("AEmpty", 1);
  const auto equal_zero_tail = grammar.addNonterminal("AZeroTail", 2);
  const auto equal_one_tail = grammar.addNonterminal("AOneTail", 2);
  const auto equal_hash = grammar.addNonterminal("AHash", 2);
  grammar.setStart(start);

  grammar.addBasic(empty, std::string(kEpsilonLabel));
  grammar.addInsert(equal, empty, std::string(kEpsilonLabel), 1);
  grammar.addAppend(equal_zero_tail, equal, "0", 1);
  grammar.addAppend(equal, equal_zero_tail, "0", 0);
  grammar.addAppend(equal_one_tail, equal, "1", 1);
  grammar.addAppend(equal, equal_one_tail, "1", 0);
  grammar.addAppend(equal_hash, equal, "#", 0);

  // S(x1 y1# y2 x2) <- A(x1, x2), AHash(y1#, y2).
  grammar.addConcatenate(start, {equal, equal_hash},
                         {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}});
  return grammar;
}

bool balancedProjection(const std::vector<std::string> &word,
                        bool parentheses) {
  std::vector<unsigned> stack;
  for (const std::string &label : word) {
    const bool relevant = parentheses ? label[1] == 'p' : label[1] == 'b';
    if (!relevant) {
      continue;
    }
    const unsigned id = static_cast<unsigned>(label.back() - '0');
    if (label[0] == 'o') {
      stack.push_back(id);
    } else if (stack.empty() || stack.back() != id) {
      return false;
    } else {
      stack.pop_back();
    }
  }
  return stack.empty();
}

std::vector<std::vector<std::string>> interleavedWords(std::size_t length) {
  const std::array<std::string, 4> alphabet = {"op--0", "cp--0", "ob--0",
                                               "cb--0"};
  std::vector<std::vector<std::string>> words;
  std::vector<std::string> word(length);
  const auto enumerate = [&](const auto &self, std::size_t index) -> void {
    if (index == length) {
      if (balancedProjection(word, true) && balancedProjection(word, false)) {
        words.push_back(word);
      }
      return;
    }
    for (const std::string &label : alphabet) {
      word[index] = label;
      self(self, index + 1);
    }
  };
  enumerate(enumerate, 0);
  return words;
}

std::size_t acceptedWords(const Grammar &grammar,
                          const std::vector<std::vector<std::string>> &words) {
  std::size_t accepted = 0;
  for (const std::vector<std::string> &word : words) {
    const ReachabilityResult result =
        Solver{}.solve(linearGraph(word), grammar);
    accepted += result.reaches(0, static_cast<Vertex>(word.size())) ? 1U : 0U;
  }
  return accepted;
}

TEST(MCFLGraphTest, ParsesArtifactDotAndDeduplicatesEdges) {
  std::istringstream input("digraph G {\n"
                           "  10 -> 20 [label=\"op--7\"];\n"
                           "  10 -> 20 [label=\"op--7\"];\n"
                           "  20 -> 30 [label=\"normal\"];\n"
                           "}\n");
  const Graph graph = Graph::parseDot(input);
  ASSERT_EQ(graph.edges().size(), 2U);
  EXPECT_EQ(graph.edges()[0], (Edge{10, 20, "op--7"}));
  EXPECT_EQ(graph.edges()[1], (Edge{20, 30, "normal"}));
}

TEST(MCFLGrammarTest, RejectsPermutingTypeFiveRules) {
  Grammar grammar;
  const auto start = grammar.addNonterminal("S", 1);
  const auto pair = grammar.addNonterminal("A", 2);
  grammar.setStart(start);
  grammar.addConcatenate(start, {pair}, {{{0, 1}, {0, 0}}});
  EXPECT_THROW(grammar.validate(), std::invalid_argument);
}

TEST(MCFLSolverTest, SupportsImplicitEpsilonPaths) {
  Graph graph;
  graph.addVertex(41);
  Grammar grammar;
  const auto start = grammar.addNonterminal("S", 1);
  grammar.setStart(start);
  grammar.addBasic(start, std::string(kEpsilonLabel));

  const ReachabilityResult result = Solver{}.solve(graph, grammar);
  EXPECT_TRUE(result.reaches(41, 41));
  ASSERT_TRUE(result.witness(41, 41).has_value());
  EXPECT_TRUE(result.witness(41, 41)->empty());
}

TEST(MCFLSolverTest, ExpandsFactsAcrossExplicitEpsilonEdges) {
  Graph graph;
  graph.addEdge(0, 1, std::string(kEpsilonLabel));
  graph.addEdge(1, 2, "x");
  graph.addEdge(2, 3, std::string(kEpsilonLabel));
  Grammar grammar;
  const auto start = grammar.addNonterminal("S", 1);
  grammar.setStart(start);
  grammar.addBasic(start, "x");

  const ReachabilityResult result = Solver{}.solve(graph, grammar);
  ASSERT_TRUE(result.reaches(0, 3));
  ASSERT_TRUE(result.witness(0, 3).has_value());
  ASSERT_EQ(result.witness(0, 3)->size(), 3U);
  EXPECT_EQ(labels(*result.witness(0, 3)), std::vector<std::string>{"x"});
}

TEST(MCFLSolverTest, ExecutesEveryNormalFormRuleAndBuildsAWitness) {
  Grammar grammar;
  const auto start = grammar.addNonterminal("S", 1);
  const auto tuple = grammar.addNonterminal("Tuple", 2);
  const auto atom = grammar.addNonterminal("Atom", 1);
  const auto appended = grammar.addNonterminal("Appended", 1);
  const auto wrapped = grammar.addNonterminal("Wrapped", 1);
  grammar.setStart(start);

  grammar.addBasic(atom, "x");
  grammar.addAppend(appended, atom, "b", 0);
  grammar.addPrepend(wrapped, appended, "a", 0);
  grammar.addInsert(tuple, wrapped, "y", 1);
  grammar.addConcatenate(start, {tuple}, {{{0, 0}, {0, 1}}});

  const std::vector<std::string> word{"a", "x", "b", "y"};
  const ReachabilityResult result = Solver{}.solve(linearGraph(word), grammar);
  ASSERT_TRUE(result.reaches(0, 4));
  ASSERT_TRUE(result.witness(0, 4).has_value());
  EXPECT_EQ(labels(*result.witness(0, 4)), word);
}

TEST(MCFLSolverTest, SolvesAProperTwoDimensionalCopyLanguage) {
  const Grammar grammar = copyLanguageGrammar();
  EXPECT_EQ(grammar.dimension(), 2U);
  EXPECT_EQ(grammar.rank(), 2U);

  const std::vector<std::string> accepted{"0", "1", "#", "1", "0"};
  const ReachabilityResult yes = Solver{}.solve(linearGraph(accepted), grammar);
  ASSERT_TRUE(yes.reaches(0, 5));
  ASSERT_TRUE(yes.witness(0, 5).has_value());
  EXPECT_EQ(labels(*yes.witness(0, 5)), accepted);

  const std::vector<std::string> rejected{"0", "1", "#", "0", "0"};
  EXPECT_FALSE(Solver{}.solve(linearGraph(rejected), grammar).reaches(0, 5));
}

TEST(MCFLSolverTest, SupportsEmptyTypeFiveOutputComponents) {
  Grammar grammar;
  const auto start = grammar.addNonterminal("S", 1);
  const auto pair = grammar.addNonterminal("Pair", 2);
  const auto atom = grammar.addNonterminal("Atom", 1);
  grammar.setStart(start);
  grammar.addBasic(atom, "x");
  grammar.addConcatenate(pair, {atom}, {{{0, 0}}, {}});
  grammar.addConcatenate(start, {pair}, {{{0, 0}, {0, 1}}});

  const ReachabilityResult result = Solver{}.solve(linearGraph({"x"}), grammar);
  ASSERT_TRUE(result.reaches(0, 1));
  ASSERT_TRUE(result.witness(0, 1).has_value());
  EXPECT_EQ(labels(*result.witness(0, 1)), std::vector<std::string>{"x"});
}

TEST(MCFLSolverTest, SupportsRankThreeJoins) {
  Grammar grammar;
  const auto start = grammar.addNonterminal("S", 1);
  const auto first = grammar.addNonterminal("First", 1);
  const auto second = grammar.addNonterminal("Second", 1);
  const auto third = grammar.addNonterminal("Third", 1);
  grammar.setStart(start);
  grammar.addBasic(first, "a");
  grammar.addBasic(second, "b");
  grammar.addBasic(third, "c");
  grammar.addConcatenate(start, {first, second, third},
                         {{{0, 0}, {1, 0}, {2, 0}}});

  const std::vector<std::string> word{"a", "b", "c"};
  const ReachabilityResult result = Solver{}.solve(linearGraph(word), grammar);
  EXPECT_EQ(grammar.rank(), 3U);
  ASSERT_TRUE(result.reaches(0, 3));
  ASSERT_TRUE(result.witness(0, 3).has_value());
  EXPECT_EQ(labels(*result.witness(0, 3)), word);
}

TEST(MCFLInterleavedGrammarTest, ReproducesPaperLengthFourCoverage) {
  const InterleavedAlphabet alphabet{{0}, {0}};
  const auto words = interleavedWords(4);
  EXPECT_EQ(words.size(), 10U);

  const Grammar simple_one = buildInterleavedDyckGrammar(
                                 1, alphabet, InterleavedGrammarVariant::Simple)
                                 .grammar;
  const Grammar simple_two = buildInterleavedDyckGrammar(
                                 2, alphabet, InterleavedGrammarVariant::Simple)
                                 .grammar;
  const Grammar full_one = buildInterleavedDyckGrammar(1, alphabet).grammar;
  const Grammar full_two = buildInterleavedDyckGrammar(2, alphabet).grammar;

  EXPECT_EQ(acceptedWords(simple_one, words), 6U);
  EXPECT_EQ(acceptedWords(simple_two, words), 10U);
  EXPECT_EQ(acceptedWords(full_one, words), 8U);
  EXPECT_EQ(acceptedWords(full_two, words), 10U);
}

TEST(MCFLInterleavedGrammarTest, ReproducesPaperLengthSixCoverage) {
  const InterleavedAlphabet alphabet{{0}, {0}};
  const auto words = interleavedWords(6);
  EXPECT_EQ(words.size(), 70U);

  const Grammar simple_one = buildInterleavedDyckGrammar(
                                 1, alphabet, InterleavedGrammarVariant::Simple)
                                 .grammar;
  const Grammar simple_two = buildInterleavedDyckGrammar(
                                 2, alphabet, InterleavedGrammarVariant::Simple)
                                 .grammar;
  const Grammar simple_three =
      buildInterleavedDyckGrammar(3, alphabet,
                                  InterleavedGrammarVariant::Simple)
          .grammar;
  const Grammar full_one = buildInterleavedDyckGrammar(1, alphabet).grammar;
  const Grammar full_two = buildInterleavedDyckGrammar(2, alphabet).grammar;
  const Grammar full_three = buildInterleavedDyckGrammar(3, alphabet).grammar;

  EXPECT_EQ(acceptedWords(simple_one, words), 18U);
  EXPECT_EQ(acceptedWords(simple_two, words), 58U);
  EXPECT_EQ(acceptedWords(simple_three, words), 70U);
  EXPECT_EQ(acceptedWords(full_one, words), 40U);
  EXPECT_EQ(acceptedWords(full_two, words), 70U);
  EXPECT_EQ(acceptedWords(full_three, words), 70U);
}

TEST(MCFLInterleavedGrammarTest, GeneratesTheRankTwoHierarchyGenerically) {
  const InterleavedAlphabet alphabet{{0, 1}, {0, 1}};
  const Grammar grammar = buildInterleavedDyckGrammar(3, alphabet).grammar;
  EXPECT_EQ(grammar.dimension(), 3U);
  EXPECT_EQ(grammar.rank(), 2U);
  EXPECT_NO_THROW(grammar.validate());
}

TEST(MCFLInterleavedSolverTest, DimensionTwoFindsACrossingWord) {
  const Graph graph = linearGraph({"op--0", "ob--0", "cp--0", "cb--0"});
  InterleavedOptions options;
  options.max_dimension = 2;
  options.condense = false;
  const InterleavedAnalysisResult result =
      InterleavedDyckSolver{}.solve(graph, options);

  ASSERT_EQ(result.dimensions.size(), 2U);
  EXPECT_EQ(result.dimensions[0].reachable_pairs.count({0, 4}), 0U);
  EXPECT_EQ(result.dimensions[1].reachable_pairs.count({0, 4}), 1U);
}

TEST(MCFLInterleavedSolverTest, DropsDelimiterTypesWithoutBothDirections) {
  const Graph graph = linearGraph({"op--0", "normal"});
  const InterleavedAnalysisResult result = InterleavedDyckSolver{}.solve(graph);
  EXPECT_EQ(result.reachablePairs().count({0, 1}), 0U);
  EXPECT_EQ(result.reachablePairs().count({1, 2}), 1U);
  EXPECT_EQ(result.reachablePairs().count({2, 1}), 0U);
}

TEST(MCFLInterleavedSolverTest,
     ArtifactExpansionReproducesCondensedCrossProduct) {
  const Graph graph = linearGraph({"normal"});
  InterleavedOptions options;
  options.max_dimension = 1;
  options.expansion_policy = CondensationExpansionPolicy::ArtifactCompatible;
  const InterleavedAnalysisResult result =
      InterleavedDyckSolver{}.solve(graph, options);
  EXPECT_EQ(result.reachablePairs().count({0, 1}), 1U);
  EXPECT_EQ(result.reachablePairs().count({1, 0}), 1U);
}

TEST(MCFLInterleavedSolverTest, DimensionTwoCollapsesAdjacentUnreachableGap) {
  Graph graph;
  graph.addEdge(0, 1, "op--0");
  graph.addEdge(1, 2, "cp--0");
  graph.addEdge(10, 11, "ob--0");
  graph.addEdge(11, 12, "cb--0");
  InterleavedOptions options;
  options.max_dimension = 2;
  options.condense = false;
  const InterleavedAnalysisResult result =
      InterleavedDyckSolver{}.solve(graph, options);
  ASSERT_EQ(result.dimensions.size(), 2U);
  EXPECT_EQ(result.dimensions[1].reachable_pairs.count({0, 2}), 1U);
  EXPECT_EQ(result.dimensions[1].reachable_pairs.count({10, 12}), 1U);
}

TEST(MCFLInterleavedSolverTest,
     DirectedNeutralEdgeDoesNotCertifyUnreachableReversePair) {
  // Audit counterexample 1 (d=1): 0->1 [normal], 1->0 [op--0], 1->2 [cp--0].
  // Pair (1, 0) has only unmatched op--0 paths and must NOT be certified.
  {
    Graph g1;
    g1.addEdge(0, 1, "normal");
    g1.addEdge(1, 0, "op--0");
    g1.addEdge(1, 2, "cp--0");

    InterleavedOptions opts;
    opts.max_dimension = 1;
    opts.condense = true;
    const InterleavedAnalysisResult res = InterleavedDyckSolver{}.solve(g1, opts);
    ASSERT_EQ(res.dimensions.size(), 1U);
    EXPECT_EQ(res.dimensions[0].reachable_pairs.count({1, 0}), 0U);
    EXPECT_EQ(res.dimensions[0].reachable_pairs.count({0, 1}), 1U);
    EXPECT_EQ(res.dimensions[0].reachable_pairs.count({1, 2}), 1U);
    EXPECT_EQ(res.dimensions[0].reachable_pairs.count({0, 2}), 1U);
  }

  // Audit counterexample 2 (d=2, both alphabets active + balanced loop at 1).
  {
    Graph g2;
    g2.addEdge(0, 1, "normal");
    g2.addEdge(1, 0, "op--0");
    g2.addEdge(1, 2, "cp--0");
    g2.addEdge(1, 3, "ob--0");
    g2.addEdge(3, 1, "cb--0");

    InterleavedOptions opts;
    opts.max_dimension = 2;
    opts.condense = true;
    const InterleavedAnalysisResult res = InterleavedDyckSolver{}.solve(g2, opts);
    ASSERT_EQ(res.dimensions.size(), 2U);
    EXPECT_EQ(res.dimensions[0].reachable_pairs.count({1, 0}), 0U);
    EXPECT_EQ(res.dimensions[1].reachable_pairs.count({1, 0}), 0U);
    EXPECT_EQ(res.dimensions[1].reachable_pairs.count({0, 2}), 1U);
  }

  // Mutual neutral cycle 0 <-> 1 MUST still be contracted and expanded soundly.
  {
    Graph g_cycle;
    g_cycle.addEdge(0, 1, "normal");
    g_cycle.addEdge(1, 0, "normal");
    g_cycle.addEdge(1, 2, "op--0");
    g_cycle.addEdge(2, 0, "cp--0");

    InterleavedOptions opts;
    opts.max_dimension = 2;
    opts.condense = true;
    const InterleavedAnalysisResult res =
        InterleavedDyckSolver{}.solve(g_cycle, opts);
    EXPECT_EQ(res.reachablePairs().count({0, 1}), 1U);
    EXPECT_EQ(res.reachablePairs().count({1, 0}), 1U);
  }
}

TEST(MCFLInterleavedSolverTest,
     SoundCondensationMatchesExactOracleOnRandomNeutralGraphs) {
  const std::array<std::string, 7> labels = {
      "normal", "op--0", "cp--0", "op--1", "cp--1", "ob--0", "cb--0"};

  auto exactReachableBounded = [](const Graph &g, Vertex src, Vertex dst) {
    using State =
        std::tuple<Vertex, std::vector<unsigned>, std::vector<unsigned>>;
    constexpr std::size_t kMaxDepth = 8;
    std::set<State> visited;
    std::queue<State> q;
    State init{src, {}, {}};
    visited.insert(init);
    q.push(std::move(init));
    while (!q.empty()) {
      auto [u, ps, bs] = q.front();
      q.pop();
      if (u == dst && ps.empty() && bs.empty()) {
        return true;
      }
      for (const Edge &e : g.edges()) {
        if (e.source != u) {
          continue;
        }
        auto nps = ps;
        auto nbs = bs;
        if (e.label == "normal") {
          // neutral
        } else if (e.label.rfind("op--", 0) == 0) {
          if (nps.size() >= kMaxDepth)
            continue;
          nps.push_back(static_cast<unsigned>(std::stoul(e.label.substr(4))));
        } else if (e.label.rfind("cp--", 0) == 0) {
          const auto id = static_cast<unsigned>(std::stoul(e.label.substr(4)));
          if (nps.empty() || nps.back() != id)
            continue;
          nps.pop_back();
        } else if (e.label.rfind("ob--", 0) == 0) {
          if (nbs.size() >= kMaxDepth)
            continue;
          nbs.push_back(static_cast<unsigned>(std::stoul(e.label.substr(4))));
        } else if (e.label.rfind("cb--", 0) == 0) {
          const auto id = static_cast<unsigned>(std::stoul(e.label.substr(4)));
          if (nbs.empty() || nbs.back() != id)
            continue;
          nbs.pop_back();
        }
        State next{e.target, std::move(nps), std::move(nbs)};
        if (visited.insert(next).second) {
          q.push(std::move(next));
        }
      }
    }
    return false;
  };

  std::mt19937 rng(0xC0FFEEU);
  for (unsigned trial = 0; trial < 60; ++trial) {
    Graph g;
    constexpr int kVertices = 5;
    for (int v = 0; v < kVertices; ++v) {
      g.addVertex(v);
    }
    std::uniform_int_distribution<int> vdist(0, kVertices - 1);
    std::uniform_int_distribution<std::size_t> ldist(0, labels.size() - 1);
    for (int e = 0; e < 9; ++e) {
      g.addEdge(vdist(rng), vdist(rng), labels[ldist(rng)]);
    }

    InterleavedOptions cond_opts;
    cond_opts.max_dimension = 2;
    cond_opts.condense = true;

    InterleavedOptions raw_opts;
    raw_opts.max_dimension = 2;
    raw_opts.condense = false;

    const auto cond_res = InterleavedDyckSolver{}.solve(g, cond_opts);
    const auto raw_res = InterleavedDyckSolver{}.solve(g, raw_opts);

    for (unsigned d = 0; d < 2; ++d) {
      // Every uncondensed pair must remain certified after condensation.
      for (const Pair &p : raw_res.dimensions[d].reachable_pairs) {
        EXPECT_NE(cond_res.dimensions[d].reachable_pairs.count(p), 0U)
            << "trial=" << trial << " dim=" << (d + 1) << " pair=(" << p.source
            << "," << p.target << ")";
      }
      // Every condensed pair must be genuinely Interleaved-Dyck reachable.
      for (const Pair &p : cond_res.dimensions[d].reachable_pairs) {
        EXPECT_NE(p.source, p.target);
        EXPECT_TRUE(exactReachableBounded(g, p.source, p.target))
            << "trial=" << trial << " dim=" << (d + 1)
            << " UNSOUND condensed pair=(" << p.source << "," << p.target
            << ")";
      }
    }
  }
}

TEST(MCFLArtifactRegressionTest, ReproducesSmallPublicTaintPairCounts) {
  struct ExpectedCounts {
    const char *filename;
    std::size_t d1;
    std::size_t d2;
  };
  constexpr std::array<ExpectedCounts, 5> kCases{{
      {"fakebanker.dot", 249U, 251U},
      {"faketaobao.dot", 57U, 59U},
      {"jollyserv.dot", 155U, 164U},
      {"loozfon.dot", 76U, 93U},
      {"uranai.dot", 143U, 143U},
  }};
  for (const ExpectedCounts &test_case : kCases) {
    const interleaved_dyck::Graph graph = interleaved_dyck::Graph::parseDotFile(
        std::string(MCFL_DATASET_DIR) + "/" + test_case.filename);
    const InterleavedAnalysisResult result =
        InterleavedDyckSolver{}.solve(graph);
    ASSERT_EQ(result.dimensions.size(), 2U) << test_case.filename;
    EXPECT_EQ(result.dimensions[0].reachable_pairs.size(), test_case.d1)
        << test_case.filename;
    EXPECT_EQ(result.dimensions[1].reachable_pairs.size(), test_case.d2)
        << test_case.filename;
  }
}

} // namespace
} // namespace lotus::cfl::interleaved_dyck::mcfl
