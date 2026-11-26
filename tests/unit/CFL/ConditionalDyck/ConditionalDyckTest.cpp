#include "CFL/ConditionalDyck/ConditionalDyck.h"

#include <algorithm>
#include <array>
#include <future>
#include <limits>
#include <random>
#include <stdexcept>

#include <gtest/gtest.h>

namespace lotus::cfl::conditional_dyck {
namespace {

constexpr std::array<Language, 4> languages = {
    Language::Balanced, Language::Left, Language::Right, Language::Extended};

// Independent whole-graph oracle: close S under concatenation and matching,
// then use Floyd-Warshall for L/R and the identity D = R L. It has no
// conditional facts, normalized Tail symbols, worklist or bridge machinery.
struct Oracle {
  std::map<Node, std::size_t> index;
  std::array<std::vector<std::vector<bool>>, 4> matrices;
  explicit Oracle(const Graph &graph) {
    for (Node node : graph.nodes())
      index.emplace(node, index.size());
    const auto n = index.size();
    for (auto &matrix : matrices)
      matrix.assign(n, std::vector<bool>(n));
    auto &s = matrices[0];
    for (std::size_t i = 0; i < n; ++i)
      s[i][i] = true;
    for (const auto &edge : graph.edges())
      if (edge.label.kind == EdgeKind::Neutral)
        s[index.at(edge.source)][index.at(edge.target)] = true;
    auto close = [n](std::vector<std::vector<bool>> &matrix) {
      bool changed = false;
      for (std::size_t k = 0; k < n; ++k)
        for (std::size_t i = 0; i < n; ++i)
          for (std::size_t j = 0; j < n; ++j)
            if (!matrix[i][j] && matrix[i][k] && matrix[k][j]) {
              matrix[i][j] = true;
              changed = true;
            }
      return changed;
    };
    bool changed = true;
    while (changed) {
      changed = close(s);
      for (const auto &open : graph.edges()) {
        if (open.label.kind != EdgeKind::Open)
          continue;
        for (const auto &end : graph.edges()) {
          if (end.label.kind != EdgeKind::Close ||
              end.label.call_site != open.label.call_site)
            continue;
          const auto a = index.at(open.source), b = index.at(end.target);
          if (!s[a][b] && s[index.at(open.target)][index.at(end.source)]) {
            s[a][b] = true;
            changed = true;
          }
        }
      }
    }
    matrices[1] = matrices[2] = s;
    for (const auto &edge : graph.edges()) {
      if (edge.label.kind == EdgeKind::Open)
        matrices[1][index.at(edge.source)][index.at(edge.target)] = true;
      if (edge.label.kind == EdgeKind::Close)
        matrices[2][index.at(edge.source)][index.at(edge.target)] = true;
    }
    close(matrices[1]);
    close(matrices[2]);
    for (std::size_t i = 0; i < n; ++i)
      for (std::size_t j = 0; j < n; ++j)
        for (std::size_t k = 0; k < n; ++k)
          matrices[3][i][j] =
              matrices[3][i][j] || (matrices[2][i][k] && matrices[1][k][j]);
  }
  bool contains(Node source, Node target, Language language) const {
    return matrices[static_cast<std::size_t>(language)][index.at(source)]
                   [index.at(target)];
  }
};

struct Example {
  Library library;
  Graph client;
  std::vector<ConditionalInput> guarded;
  void guard(Node source, Node target, Label label, Condition condition) {
    guarded.push_back({{source, target, label}, condition});
    library.addConditionalEdge(source, target, label, condition);
  }
  Graph whole(const std::set<Condition> &enabled = {}) const {
    Graph result = library.graph();
    for (Node node : client.nodes())
      result.addNode(node);
    for (const auto &edge : client.edges())
      result.addEdge(edge.source, edge.target, edge.label);
    for (const auto &input : guarded)
      if (enabled.count(input.condition))
        result.addEdge(input.edge.source, input.edge.target, input.edge.label);
    return result;
  }
};

void compare(const Example &example, const Summary &summary,
             const std::set<Condition> &enabled = {}) {
  const Oracle expected(example.whole(enabled));
  const auto actual = Solver{}.analyze(summary, example.client, enabled);
  for (Node source : example.client.nodes()) {
    if (example.library.graph().nodes().count(source))
      continue;
    for (Node target : example.client.nodes()) {
      if (example.library.graph().nodes().count(target))
        continue;
      for (Language language : languages)
        EXPECT_EQ(actual.contains(source, target, language),
                  expected.contains(source, target, language))
            << "source=" << source << " target=" << target
            << " language=" << static_cast<int>(language);
    }
  }
  // Queries on library nodes are deliberately outside the exported contract.
  for (Language language : languages)
    for (const auto &pair : actual.pairs(language)) {
      EXPECT_EQ(example.library.graph().nodes().count(pair.first), 0U);
      EXPECT_EQ(example.library.graph().nodes().count(pair.second), 0U);
    }
}

Example twoCallbacks(unsigned mask) {
  Example example;
  for (Node source : {0, 2, 4})
    example.library.graph().addEdge(source, source + 1);
  example.library.markBoundary(0, Boundary::Input);
  example.library.markBoundary(5, Boundary::Output);
  example.library.addCallback(1, 2);
  example.library.addCallback(3, 4);
  auto &client = example.client;
  client.addEdge(6, 0, Label::open(10));
  client.addEdge(5, 7, Label::close(10));
  client.addEdge(1, 8, Label::open(11));
  client.addEdge(9, 2, Label::close(11));
  client.addEdge(3, 10, Label::open(12));
  client.addEdge(11, 4, Label::close(12));
  if (mask & 1)
    client.addEdge(8, 9);
  if (mask & 2)
    client.addEdge(10, 11);
  return example;
}

TEST(ConditionalDyckTest, WholeGraphMatchesIndependentOracle) {
  std::mt19937 random(17001);
  for (int sample = 0; sample < 40; ++sample) {
    Graph graph;
    for (Node node = 0; node < 7; ++node)
      graph.addNode(node);
    for (int i = 0; i < 12; ++i) {
      const auto kind = random() % 3;
      Label label = kind == 0   ? Label::neutral()
                    : kind == 1 ? Label::open(random() % 3)
                                : Label::close(random() % 3);
      graph.addEdge(random() % 7, random() % 7, label);
    }
    const Oracle oracle(graph);
    const auto result = Solver{}.solve(graph);
    for (Node source : graph.nodes())
      for (Node target : graph.nodes())
        for (Language language : languages)
          ASSERT_EQ(result.contains(source, target, language),
                    oracle.contains(source, target, language))
              << sample;
  }
}

TEST(ConditionalDyckTest, FullAndBoundedCallbackSummariesMatchWholeProgram) {
  for (auto bound : {std::optional<std::size_t>{}, {1}, {2}, {7}}) {
    const auto summary = Solver{}.summarize(twoCallbacks(0).library, {bound});
    if (bound) {
      EXPECT_LE(summary.statistics().largest_premise_set, *bound);
      if (*bound == 1)
        EXPECT_GT(summary.statistics().bridging_facts, 0U);
    }
    for (unsigned mask = 0; mask < 4; ++mask) {
      SCOPED_TRACE(mask);
      const auto example = twoCallbacks(mask);
      compare(example, summary);
      EXPECT_EQ(Solver{}.analyze(summary, example.client).contains(6, 7),
                mask == 3);
    }
  }
}

TEST(ConditionalDyckTest, KeepsAlternativeBridgeDerivations) {
  Example example;
  example.library.markBoundary(0, Boundary::Input);
  example.library.markBoundary(4, Boundary::Output);
  example.guard(0, 1, Label::neutral(), 1);
  example.guard(1, 4, Label::neutral(), 2);
  example.guard(0, 2, Label::neutral(), 3);
  example.guard(2, 4, Label::neutral(), 4);
  example.client.addEdge(10, 0, Label::open(100));
  example.client.addEdge(4, 11, Label::close(100));
  const auto summary = Solver{}.summarize(example.library);
  EXPECT_GT(summary.statistics().bridging_facts, 0U);
  for (unsigned mask = 0; mask < 16; ++mask) {
    std::set<Condition> enabled;
    for (unsigned i = 0; i < 4; ++i)
      if (mask & (1U << i))
        enabled.insert(i + 1);
    compare(example, summary, enabled);
    EXPECT_EQ(
        Solver{}.analyze(summary, example.client, enabled).contains(10, 11),
        (mask & 3) == 3 || (mask & 12) == 12);
  }
}

TEST(ConditionalDyckTest, HandlesSharedConditionsAndFullWidthIdentifiers) {
  const Node big = std::numeric_limits<Node>::max();
  const Condition guard = std::numeric_limits<Condition>::max();
  Example example;
  example.library.markBoundary(big, Boundary::Input);
  example.library.markBoundary(big - 3, Boundary::Output);
  example.guard(big, big - 1, Label::open(big), guard);
  example.library.graph().addEdge(big - 1, big - 2);
  example.guard(big - 2, big - 3, Label::close(big), guard);
  example.client.addEdge(10, big, Label::open(0));
  example.client.addEdge(big - 3, 11, Label::close(0));
  for (auto bound : {std::optional<std::size_t>{}, {1}}) {
    const auto summary = Solver{}.summarize(example.library, {bound});
    compare(example, summary);
    compare(example, summary, {guard});
    EXPECT_FALSE(Solver{}.analyze(summary, example.client).contains(10, 11));
    EXPECT_TRUE(
        Solver{}.analyze(summary, example.client, {guard}).contains(10, 11));
    EXPECT_LE(summary.statistics().largest_premise_set, 1U);
  }
}

TEST(ConditionalDyckTest, RecursiveCallbacksNeedAnActualBaseCase) {
  Example example;
  example.library.graph().addEdge(0, 1);
  example.library.graph().addEdge(2, 3);
  example.library.markBoundary(0, Boundary::Input);
  example.library.markBoundary(3, Boundary::Output);
  example.library.addCallback(1, 2);
  example.client.addEdge(4, 0, Label::open(10));
  example.client.addEdge(3, 5, Label::close(10));
  example.client.addEdge(1, 6, Label::open(11));
  example.client.addEdge(7, 2, Label::close(11));
  example.client.addEdge(6, 0, Label::open(12));
  example.client.addEdge(3, 7, Label::close(12));
  const auto summary = Solver{}.summarize(example.library);
  compare(example, summary);
  EXPECT_FALSE(Solver{}.analyze(summary, example.client).contains(4, 5));
  example.client.addEdge(6, 7);
  compare(example, summary);
  EXPECT_TRUE(Solver{}.analyze(summary, example.client).contains(4, 5));
}

TEST(ConditionalDyckTest, RetainsBridgesThroughNormalizedCallMatching) {
  Example example;
  example.library.markBoundary(0, Boundary::Input);
  example.library.markBoundary(3, Boundary::Output);
  example.guard(0, 1, Label::open(1), 1);
  example.guard(1, 2, Label::neutral(), 2);
  example.guard(2, 3, Label::close(1), 3);
  example.client.addEdge(10, 0, Label::open(2));
  example.client.addEdge(3, 11, Label::close(2));
  const auto summary = Solver{}.summarize(example.library);
  EXPECT_GT(summary.statistics().bridging_facts, 0U);
  for (unsigned mask = 0; mask < 8; ++mask) {
    std::set<Condition> enabled;
    for (unsigned i = 0; i < 3; ++i)
      if (mask & (1U << i))
        enabled.insert(i + 1);
    compare(example, summary, enabled);
    EXPECT_EQ(
        Solver{}.analyze(summary, example.client, enabled).contains(10, 11),
        mask == 7);
  }
}

TEST(ConditionalDyckTest, SupportsLargePremiseSetsAndBoundsAboveFive) {
  for (auto bound : {std::optional<std::size_t>{}, {1}, {6}}) {
    // Exercise full sets with more than 32 premises and k > 5 separately:
    // combining a large graph with large k has combinatorial cost by design.
    const Node length = bound ? 8 : 34;
    Example example;
    example.library.markBoundary(0, Boundary::Input);
    example.library.markBoundary(length, Boundary::Output);
    std::set<Condition> enabled;
    for (Node node = 0; node < length; ++node) {
      example.guard(node, node + 1, Label::neutral(), node);
      enabled.insert(node);
    }
    example.client.addEdge(100, 0, Label::open(10));
    example.client.addEdge(length, 101, Label::close(10));
    const auto summary = Solver{}.summarize(example.library, {bound});
    if (bound)
      EXPECT_LE(summary.statistics().largest_premise_set, *bound);
    else
      EXPECT_EQ(summary.statistics().largest_premise_set, 34U);
    compare(example, summary, enabled);
    EXPECT_TRUE(
        Solver{}.analyze(summary, example.client, enabled).contains(100, 101));
    auto missing = enabled;
    missing.erase(length / 2);
    compare(example, summary, missing);
    EXPECT_FALSE(
        Solver{}.analyze(summary, example.client, missing).contains(100, 101));
  }
}

TEST(ConditionalDyckTest, EmptyPathsAndUnmatchedParentheses) {
  Example example;
  example.library.markBoundary(0, Boundary::Input);
  example.library.markBoundary(0, Boundary::Output);
  example.client.addEdge(1, 0, Label::open(10));
  example.client.addEdge(0, 2, Label::close(10));
  example.client.addNode(3);
  const auto summary = Solver{}.summarize(example.library);
  compare(example, summary);
  const auto result = Solver{}.analyze(summary, example.client);
  EXPECT_TRUE(result.contains(1, 2));
  for (Language language : languages)
    EXPECT_TRUE(result.contains(3, 3, language));
  Graph unmatched;
  unmatched.addEdge(0, 1, Label::close(9));
  unmatched.addEdge(1, 2, Label::open(8));
  const auto other = Solver{}.solve(unmatched);
  EXPECT_TRUE(other.contains(0, 2, Language::Extended));
  EXPECT_FALSE(other.contains(0, 2));
  EXPECT_FALSE(other.contains(0, 2, Language::Left));
  EXPECT_FALSE(other.contains(0, 2, Language::Right));
  EXPECT_TRUE(Solver{}.solve(Graph{}).pairs(Language::Balanced).empty());
  EXPECT_TRUE(
      Solver{}.analyze(summary, Graph{}).pairs(Language::Balanced).empty());
}

TEST(ConditionalDyckTest, SummaryOwnsDataAndSupportsConcurrentReuse) {
  const auto summary = Solver{}.summarize(twoCallbacks(0).library);
  const auto copy = summary;
  std::vector<std::future<bool>> tasks;
  for (unsigned i = 0; i < 12; ++i) {
    tasks.push_back(std::async(std::launch::async, [copy, i] {
      const auto example = twoCallbacks(i % 4);
      return Solver{}.analyze(copy, example.client).contains(6, 7) ==
             (i % 4 == 3);
    }));
  }
  for (auto &task : tasks)
    EXPECT_TRUE(task.get());
  EXPECT_EQ(copy.statistics().summary_facts,
            summary.statistics().summary_facts);
}

TEST(ConditionalDyckTest, RejectsInvalidContracts) {
  Solver solver;
  Library library;
  library.markBoundary(0, Boundary::Input);
  library.markBoundary(1, Boundary::Output);
  library.graph().addEdge(0, 1, Label::open(10));
  EXPECT_THROW(solver.summarize(library, {0}), std::invalid_argument);
  EXPECT_THROW(library.markBoundary(0, static_cast<Boundary>(99)),
               std::invalid_argument);
  EXPECT_THROW(library.graph().addEdge(0, 1, {EdgeKind::Neutral, 1}),
               std::invalid_argument);
  const auto summary = solver.summarize(library);
  Graph client;
  client.addEdge(2, 0);
  EXPECT_THROW(solver.analyze(summary, client), std::invalid_argument);
  client = Graph{};
  client.addEdge(2, 1, Label::open(11));
  EXPECT_THROW(solver.analyze(summary, client), std::invalid_argument);
  client = Graph{};
  client.addEdge(2, 0, Label::open(10));
  EXPECT_THROW(solver.analyze(summary, client), std::invalid_argument);
  client = Graph{};
  client.addEdge(0, 1);
  EXPECT_THROW(solver.analyze(summary, client), std::invalid_argument);
  EXPECT_THROW(solver.analyze(summary, Graph{}, {42}), std::invalid_argument);
  library.markBoundary(0, Boundary::Call);
  library.markBoundary(1, Boundary::Return);
  client = Graph{};
  client.addEdge(0, 2, Label::open(12));
  client.addEdge(3, 1, Label::close(12));
  EXPECT_THROW(solver.analyze(solver.summarize(library), client),
               std::invalid_argument);
}

TEST(ConditionalDyckTest, RandomSummariesMatchIndependentWholeProgramOracle) {
  std::mt19937 random(2017);
  for (unsigned sample = 0; sample < 20; ++sample) {
    Example example;
    for (Node node = 0; node < 5; ++node)
      example.library.graph().addNode(node);
    example.library.markBoundary(0, Boundary::Input);
    example.library.markBoundary(4, Boundary::Output);
    example.library.addCallback(1, 3);
    for (unsigned i = 0; i < 7; ++i) {
      const Node source = random() % 5, target = random() % 5;
      const unsigned kind = random() % 3;
      Label label = kind == 0   ? Label::neutral()
                    : kind == 1 ? Label::open(random() % 2)
                                : Label::close(random() % 2);
      if (i < 2)
        example.guard(source, target, label, i);
      else
        example.library.graph().addEdge(source, target, label);
    }
    example.client.addEdge(100, 0, Label::open(10));
    example.client.addEdge(4, 101, Label::close(10));
    example.client.addEdge(1, 102, Label::open(11));
    example.client.addEdge(103, 3, Label::close(11));
    for (unsigned i = 0; i < 4; ++i)
      example.client.addEdge(100 + random() % 4, 100 + random() % 4);
    for (auto bound : {std::optional<std::size_t>{}, {1}, {2}}) {
      const auto summary = Solver{}.summarize(example.library, {bound});
      for (unsigned mask = 0; mask < 4; ++mask) {
        SCOPED_TRACE(sample);
        SCOPED_TRACE(mask);
        std::set<Condition> enabled;
        if (mask & 1)
          enabled.insert(0);
        if (mask & 2)
          enabled.insert(1);
        compare(example, summary, enabled);
      }
    }
  }
}

} // namespace
} // namespace lotus::cfl::conditional_dyck
