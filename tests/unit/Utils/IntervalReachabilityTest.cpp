#include "Utils/Algorithms/Graph/IntervalReachability.h"

#include <numeric>
#include <random>
#include <set>

#include <gtest/gtest.h>

using namespace lotus::graph;
namespace {
std::vector<bool> bfs(const Adjacency &graph, Vertex root) {
  std::vector<bool> seen(graph.size(), false);
  std::vector<Vertex> work{root};
  seen[root] = true;
  while (!work.empty()) {
    auto v = work.back();
    work.pop_back();
    for (auto next : graph[v])
      if (!seen[next]) {
        seen[next] = true;
        work.push_back(next);
      }
  }
  return seen;
}
TEST(IntervalReachabilityTest, CyclesDiamondDuplicateEdgesAndIsolatedVertices) {
  Adjacency graph{{1, 2, 1}, {3}, {3}, {4}, {3}, {}, {6}};
  IntervalReachabilityIndex index(graph);
  EXPECT_EQ(index.numVertices(), 7U);
  EXPECT_EQ(index.numSCCs(), 6U);
  auto components = index.components();
  EXPECT_EQ(components->componentOf(3), components->componentOf(4));
  EXPECT_TRUE(components->cyclic(components->componentOf(3)));
  EXPECT_TRUE(components->cyclic(components->componentOf(6)));
  EXPECT_FALSE(components->cyclic(components->componentOf(5)));
  for (Vertex from = 0; from < graph.size(); ++from) {
    auto expected = bfs(graph, from);
    std::set<Vertex> enumerated;
    index.forEachReachableTarget(from, [&](Vertex v) { enumerated.insert(v); });
    for (Vertex to = 0; to < graph.size(); ++to) {
      EXPECT_EQ(index.reaches(from, to), expected[to]);
      EXPECT_EQ(enumerated.count(to) != 0, expected[to]);
    }
    const auto &ranges = index.intervalsFrom(from);
    for (size_t i = 1; i < ranges.size(); ++i)
      EXPECT_LT(ranges[i - 1].end, ranges[i].begin);
  }
  for (Vertex c = 0; c < components->size(); ++c)
    for (auto next : components->successors(c)) {
      EXPECT_LT(next, c);
      auto &callers = components->predecessors(next);
      EXPECT_NE(std::find(callers.begin(), callers.end(), c), callers.end());
    }
}
TEST(IntervalReachabilityTest, RootsAndTargetsAreIndependentRestrictions) {
  Adjacency graph{{1}, {2}, {1, 3}, {}, {3}};
  std::vector<bool> targets{false, false, true, true, true};
  IntervalReachabilityIndex index(graph, {1}, targets);
  EXPECT_EQ(index.numTargets(), 2U);
  EXPECT_FALSE(index.isIndexed(0));
  EXPECT_FALSE(index.isIndexed(4));
  EXPECT_TRUE(index.isIndexed(1));
  EXPECT_FALSE(index.isTarget(1));
  EXPECT_TRUE(index.reaches(1, 2));
  EXPECT_TRUE(index.reaches(2, 2));
  EXPECT_TRUE(index.reaches(1, 3));
  EXPECT_FALSE(index.reaches(0, 3));
  EXPECT_FALSE(index.reaches(1, 1));
  EXPECT_FALSE(index.reaches(1, 4));
  EXPECT_FALSE(index.rankOf(99));
  EXPECT_FALSE(index.reaches(99, 1));
  IntervalReachabilityIndex none(graph, {});
  EXPECT_EQ(none.numTargets(), 0U);
  EXPECT_FALSE(none.isIndexed(1));
  IntervalReachabilityIndex no_targets(graph, {0}, std::vector<bool>(5, false));
  EXPECT_TRUE(no_targets.isIndexed(1));
  EXPECT_EQ(no_targets.numTargets(), 0U);
}
TEST(IntervalReachabilityTest, EmptyAndInvalidInputs) {
  IntervalReachabilityIndex empty(Adjacency{});
  EXPECT_EQ(empty.numVertices(), 0U);
  EXPECT_EQ(empty.numIntervals(), 0U);
  EXPECT_FALSE(empty.reaches(0, 0));
  EXPECT_THROW(IntervalReachabilityIndex(Adjacency{{1}}), std::out_of_range);
  EXPECT_THROW(IntervalReachabilityIndex(Adjacency{{}}, {1}),
               std::out_of_range);
  EXPECT_THROW(IntervalReachabilityIndex(Adjacency{{}}, {0}, {true, false}),
               std::invalid_argument);
  EXPECT_THROW(IntervalReachabilityIndex(std::shared_ptr<const SCCGraph>{}),
               std::invalid_argument);
}
TEST(IntervalReachabilityTest, RandomGraphsMatchBfsAndSccMutualReachability) {
  std::mt19937 random(4371);
  for (unsigned trial = 0; trial < 100; ++trial) {
    Adjacency graph(1 + random() % 35);
    for (Vertex from = 0; from < graph.size(); ++from)
      for (Vertex to = 0; to < graph.size(); ++to)
        if (random() % 11 == 0)
          graph[from].push_back(to);
    IntervalReachabilityIndex index(graph);
    auto scc = index.components();
    std::vector<std::vector<bool>> expected;
    for (Vertex v = 0; v < graph.size(); ++v)
      expected.push_back(bfs(graph, v));
    std::vector<bool> targets(graph.size());
    for (Vertex v = 0; v < graph.size(); ++v)
      targets[v] = random() % 2;
    Vertex root = random() % graph.size();
    IntervalReachabilityIndex restricted(graph, {root}, targets);
    for (Vertex from = 0; from < graph.size(); ++from)
      for (Vertex to = 0; to < graph.size(); ++to) {
        ASSERT_EQ(index.reaches(from, to), expected[from][to]);
        ASSERT_EQ(scc->componentOf(from) == scc->componentOf(to),
                  expected[from][to] && expected[to][from]);
        ASSERT_EQ(restricted.reaches(from, to),
                  expected[root][from] && targets[to] && expected[from][to]);
      }
  }
}
TEST(IntervalReachabilityTest,
     DeepChainUsesIterativeTraversalAndCompactRanges) {
  Adjacency graph(20000);
  for (Vertex v = 1; v < graph.size(); ++v)
    graph[v - 1].push_back(v);
  auto scc = std::make_shared<SCCGraph>(graph);
  IntervalReachabilityIndex index(scc);
  EXPECT_EQ(index.components(), scc);
  EXPECT_EQ(index.numIntervals(), graph.size());
  EXPECT_TRUE(index.reaches(0, 19999));
  EXPECT_FALSE(index.reaches(19999, 0));
}
} // namespace
