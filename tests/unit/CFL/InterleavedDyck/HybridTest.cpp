#include "CFL/InterleavedDyck/Hybrid/Solver.h"
#include "CFL/InterleavedDyck/SPDS/DyckReachability.h"
#include "CFL/InterleavedDyck/SPDS/Solver.h"
#include "CFL/InterleavedDyck/StagedBounds/Solver.h"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace lotus::cfl::interleaved_dyck::hybrid {
namespace {

bool isSubset(const PairSet &sub, const PairSet &sup) {
  for (const Pair &p : sub)
    if (sup.count(p) == 0U)
      return false;
  return true;
}

TEST(InterleavedDyckHybridTest, ShortCircuitsOnEmptyAndCertifiedLowerBound) {
  Graph g;
  g.addEdge(0, 1, Label::openParenthesis(1));
  g.addEdge(1, 2, Label::openBracket(2));
  g.addEdge(2, 3, Label::closeBracket(2));
  g.addEdge(3, 4, Label::closeParenthesis(1)); // union-Dyck nested witness!

  Demands empty_dem;
  empty_dem.pairs = {{0, 3}}; // unbalanced
  const auto empty_res = Solver{}.analyzeDemands(g, empty_dem);
  EXPECT_TRUE(empty_res.stage1_spds.empty());
  EXPECT_TRUE(empty_res.final_upper.empty());
  EXPECT_EQ(empty_res.statistics.short_circuited_after, 1U);

  Demands cert_dem;
  cert_dem.pairs = {{0, 4}};
  const auto cert_res = Solver{}.analyzeDemands(g, cert_dem);
  EXPECT_EQ(cert_res.certified_lower.count({0, 4}), 1U);
  EXPECT_EQ(cert_res.final_upper.count({0, 4}), 1U);
  EXPECT_EQ(cert_res.statistics.short_circuited_after, 2U);
  EXPECT_FALSE(cert_res.statistics.affine_executed);
}

TEST(InterleavedDyckHybridTest, PreservesCrossingWitnessAndPrunesDisjointPaths) {
  Graph g;
  // Genuine crossing InterDyck path 10 -> 14: op7 ob9 cp7 cb9 (not union-Dyck!)
  g.addEdge(10, 11, Label::openParenthesis(7));
  g.addEdge(11, 12, Label::openBracket(9));
  g.addEdge(12, 13, Label::closeParenthesis(7));
  g.addEdge(13, 14, Label::closeBracket(9));

  // False positive 0 -> 4: two disjoint paths where all delimiters are matched
  // in the corridor, so Stage 1 SPDS retains (0, 4), while Stage 3 rejects it!
  // Path A (call-valid op1 cp1 op2 cp2, field-invalid ob1 cb2):
  g.addEdge(0, 1, Label::openParenthesis(1));
  g.addEdge(1, 2, Label::openBracket(1));
  g.addEdge(2, 3, Label::closeParenthesis(1));
  g.addEdge(3, 15, Label::openParenthesis(2));
  g.addEdge(15, 16, Label::closeParenthesis(2));
  g.addEdge(16, 4, Label::closeBracket(2));

  // Path B (field-valid ob1 cb1 ob2 cb2, call-invalid op1 cp2):
  g.addEdge(0, 5, Label::openParenthesis(1));
  g.addEdge(5, 6, Label::openBracket(1));
  g.addEdge(6, 7, Label::closeBracket(1));
  g.addEdge(7, 17, Label::openBracket(2));
  g.addEdge(17, 18, Label::closeParenthesis(2));
  g.addEdge(18, 4, Label::closeBracket(2));

  Demands demands;
  demands.pairs = {{10, 14}, {0, 4}};
  const auto res = Solver{}.analyzeDemands(g, demands);
  EXPECT_EQ(res.statistics.matrix_dimension, 64U);
  EXPECT_TRUE(res.statistics.affine_executed);
  // (10, 14) is not in union-Dyck lower bound, but must survive!
  EXPECT_EQ(res.certified_lower.count({10, 14}), 0U);
  EXPECT_EQ(res.final_upper.count({10, 14}), 1U);
  // (0, 4) survives SPDS (Stage 1), but is rejected by Stage 3 AffineSPDS!
  EXPECT_EQ(res.stage1_spds.count({0, 4}), 1U);
  EXPECT_EQ(res.stage3_affine.count({0, 4}), 0U);
  EXPECT_EQ(res.final_upper.count({0, 4}), 0U);
}

TEST(InterleavedDyckHybridTest,
     MonotoneChainAndLowerBoundAgreementOnRandomGraphs) {
  std::mt19937 rng(0x4B1D26);
  const Label labels[] = {
      Label::neutral(),           Label::openParenthesis(0),
      Label::closeParenthesis(0), Label::openParenthesis(1),
      Label::closeParenthesis(1), Label::openParenthesis(9),
      Label::closeParenthesis(9), Label::openBracket(0),
      Label::closeBracket(0),     Label::openBracket(2),
      Label::closeBracket(2),     Label::openBracket(10),
      Label::closeBracket(10)};

  for (int trial = 0; trial < 50; ++trial) {
    Graph g;
    for (Vertex v = 0; v < 6; ++v)
      g.addVertex(v);
    for (int e = 0; e < 16; ++e)
      g.addEdge(static_cast<Vertex>(rng() % 6), static_cast<Vertex>(rng() % 6),
                labels[rng() % 13]);

    const auto full_sb = staged_bounds::Solver{}.analyze(g);
    const auto full_hyb = Solver{}.analyze(g);

    // Demand union-Dyck PDS lower bound must equal StagedBounds union-Dyck LB!
    EXPECT_EQ(full_hyb.certified_lower, full_sb.underapproximation);
    // Hybrid strips unmatched delimiters before Stage 1, so stage1_spds is a
    // sound subset of StagedBounds's pre-trim intersection.
    EXPECT_TRUE(isSubset(full_hyb.stage1_spds, full_sb.intersection));
    EXPECT_TRUE(isSubset(full_hyb.certified_lower, full_hyb.final_upper));
    EXPECT_EQ(full_hyb.final_upper, full_hyb.stage3_affine);
    EXPECT_TRUE(isSubset(full_hyb.stage3_affine, full_hyb.stage1_spds));

    Demands dem;
    dem.sources = {0, 2};
    dem.pairs = {{1, 4}, {3, 5}};
    const auto dem_hyb = Solver{}.analyzeDemands(g, dem);
    for (const Pair &p : full_sb.underapproximation) {
      if (p.source == 0 || p.source == 2 ||
          (p.source == 1 && p.target == 4) ||
          (p.source == 3 && p.target == 5)) {
        EXPECT_EQ(dem_hyb.certified_lower.count(p), 1U);
        EXPECT_EQ(dem_hyb.final_upper.count(p), 1U);
      }
    }
  }
}

TEST(InterleavedDyckHybridTest,
     CondensedRhsDyckMatchesSpdsAndUnionDyckExactly) {
  std::mt19937 rng(0xC0DE22);
  const Label labels[] = {
      Label::neutral(),           Label::openParenthesis(0),
      Label::closeParenthesis(0), Label::openParenthesis(1),
      Label::closeParenthesis(1), Label::openBracket(0),
      Label::closeBracket(0),     Label::openBracket(1),
      Label::closeBracket(1)};

  for (int trial = 0; trial < 100; ++trial) {
    const Vertex n = static_cast<Vertex>(8 + (trial % 9)); // 8..16 vertices
    Graph g;
    for (Vertex v = 0; v < n; ++v)
      g.addVertex(v);
    // Dense neutral cycle + hub-triggering fan-in/fan-out open/close edges.
    for (Vertex v = 0; v < n; ++v) {
      if (rng() % 2 == 0)
        g.addEdge(v, (v + 1) % n, Label::neutral());
    }
    const Vertex mid = n / 2;
    for (Vertex u = 0; u < 4; ++u)
      g.addEdge(u, mid, Label::openParenthesis(0));
    for (Vertex w = n - 4; w < n; ++w)
      g.addEdge(mid, w, Label::closeParenthesis(0));
    for (int e = 0; e < 24; ++e) {
      g.addEdge(static_cast<Vertex>(rng() % n), static_cast<Vertex>(rng() % n),
                labels[rng() % 9]);
    }

    const auto spds_prep = spds::Solver{}.prepare(g);
    const auto spds_all = spds_prep.analyzeAll();
    const auto union_ref = staged_bounds::Solver{}.underapproximation(g);

    std::vector<Pair> all_pairs;
    for (Vertex u = 0; u < n; ++u)
      for (Vertex v = 0; v < n; ++v)
        all_pairs.push_back({u, v});

    const auto rhs_par = spds::filterDyckReachableDemands(
        g, all_pairs, spds::DyckProjectionKind::Parentheses, true);
    const auto rhs_brk = spds::filterDyckReachableDemands(
        g, all_pairs, spds::DyckProjectionKind::Brackets, true);
    const auto rhs_union = spds::filterDyckReachableDemands(
        g, all_pairs, spds::DyckProjectionKind::UnionDyck, false);
    const auto rhs_inter =
        spds::filterInterleavedBooleanDemands(g, all_pairs, true);
    const auto rhs_inter_all = spds::filterInterleavedBooleanAll(g, true);

    EXPECT_EQ(rhs_par, spds_all.parenthesis_pairs);
    EXPECT_EQ(rhs_brk, spds_all.bracket_pairs);
    EXPECT_EQ(rhs_union, union_ref);
    EXPECT_EQ(rhs_inter, spds_all.upper_bound);
    EXPECT_EQ(rhs_inter_all, spds_all.upper_bound);

    // Post-skewed demands (2 sources x 6 targets) and Pre-skewed (6 sources x
    // 2 targets)
    std::vector<Pair> post_demands, pre_demands;
    for (Vertex s = 0; s < 2; ++s)
      for (Vertex t = 2; t < 8; ++t)
        post_demands.push_back({s, t});
    for (Vertex s = 2; s < 8; ++s)
      for (Vertex t = 0; t < 2; ++t)
        pre_demands.push_back({s, t});

    EXPECT_EQ(
        spds::filterInterleavedBooleanDemands(g, post_demands, false),
        spds_prep.analyzeDemands(post_demands, spds::DemandDirection::Post)
            .upper_bound);
    EXPECT_EQ(
        spds::filterInterleavedBooleanDemands(g, pre_demands, false),
        spds_prep.analyzeDemands(pre_demands, spds::DemandDirection::Pre)
            .upper_bound);

    // Source and target anchor queries
    for (Vertex a : {Vertex{0}, mid}) {
      const auto post_par = spds::queryDyckReachableFromAnchor(
          g, a, spds::Direction::Post, spds::DyckProjectionKind::Parentheses);
      PairSet post_set;
      for (Vertex v : post_par)
        post_set.insert({a, v});
      EXPECT_EQ(post_set, spds_prep.analyzeFrom(a).parenthesis_pairs);

      const auto pre_brk = spds::queryDyckReachableFromAnchor(
          g, a, spds::Direction::Pre, spds::DyckProjectionKind::Brackets);
      PairSet pre_set;
      for (Vertex v : pre_brk)
        pre_set.insert({v, a});
      EXPECT_EQ(pre_set, spds_prep.analyzeTo(a).bracket_pairs);
    }

    // Hybrid analyzeDemands with mixed pairs, sources, targets matches spds
    Demands mix;
    mix.pairs = post_demands;
    mix.sources = {0};
    mix.targets = {n - 1};
    const auto hyb_res = Solver{}.analyzeDemands(g, mix);
    EXPECT_TRUE(isSubset(hyb_res.certified_lower, hyb_res.final_upper));
    EXPECT_TRUE(isSubset(hyb_res.final_upper, hyb_res.stage1_spds));
    for (const Pair &p : union_ref) {
      if (p.source != p.target &&
          (p.source == 0 || p.target == n - 1 ||
           (p.source < 2 && p.target >= 2 && p.target < 8))) {
        EXPECT_EQ(hyb_res.certified_lower.count(p), 1U);
      }
    }
  }
}

TEST(InterleavedDyckHybridTest, BitParallelCorridorSlicingMatchesPerPairBfs) {
  std::mt19937 rng(0xB17DA9);
  Graph g;
  constexpr Vertex N = 120;
  for (Vertex v = 0; v < N; ++v)
    g.addVertex(v);
  for (int e = 0; e < 350; ++e) {
    const Vertex u = static_cast<Vertex>(rng() % N);
    const Vertex v = static_cast<Vertex>(rng() % N);
    g.addEdge(u, v,
              (e % 5 == 0) ? Label::neutral()
                           : Label::openParenthesis(static_cast<unsigned>(e % 7)));
  }

  PairSet cand_pairs;
  Demands demands;
  for (Vertex s = 0; s < 90; ++s) {
    const Vertex t1 = static_cast<Vertex>((s * 7 + 13) % N);
    const Vertex t2 = static_cast<Vertex>((s * 11 + 29) % N);
    cand_pairs.insert({s, t1});
    cand_pairs.insert({s, t2});
    demands.pairs.push_back({s, t1});
    demands.pairs.push_back({s, t2});
  }
  demands.pairs.push_back({5, 5}); // reflexive check
  cand_pairs.insert({5, 5});

  auto refSlice = [&](const Graph &graph, const PairSet &pairs,
                      bool ignore_self,
                      const std::vector<Vertex> &extra_sources,
                      const std::vector<Vertex> &extra_targets) {
    std::unordered_map<Vertex, std::vector<Vertex>> fwd, rev;
    for (const Edge &e : graph.edges()) {
      fwd[e.source].push_back(e.target);
      rev[e.target].push_back(e.source);
    }
    auto bfs = [&](const std::unordered_map<Vertex, std::vector<Vertex>> &adj,
                   const std::vector<Vertex> &seeds) {
      std::unordered_set<Vertex> seen;
      std::deque<Vertex> q;
      for (Vertex s : seeds)
        if (seen.insert(s).second)
          q.push_back(s);
      while (!q.empty()) {
        Vertex cur = q.front();
        q.pop_front();
        auto it = adj.find(cur);
        if (it != adj.end())
          for (Vertex nxt : it->second)
            if (seen.insert(nxt).second)
              q.push_back(nxt);
      }
      return seen;
    };
    std::vector<bool> keep(graph.edges().size(), false);
    for (const Pair &p : pairs) {
      if (ignore_self && p.source == p.target)
        continue;
      auto f = bfs(fwd, {p.source});
      auto r = bfs(rev, {p.target});
      for (std::size_t i = 0; i < graph.edges().size(); ++i)
        if (f.count(graph.edges()[i].source) && r.count(graph.edges()[i].target))
          keep[i] = true;
    }
    if (!extra_sources.empty()) {
      auto f = bfs(fwd, extra_sources);
      for (std::size_t i = 0; i < graph.edges().size(); ++i)
        if (f.count(graph.edges()[i].source))
          keep[i] = true;
    }
    if (!extra_targets.empty()) {
      auto r = bfs(rev, extra_targets);
      for (std::size_t i = 0; i < graph.edges().size(); ++i)
        if (r.count(graph.edges()[i].target))
          keep[i] = true;
    }
    Graph out;
    for (std::size_t i = 0; i < graph.edges().size(); ++i)
      if (keep[i])
        out.addEdge(graph.edges()[i].source, graph.edges()[i].target,
                    graph.edges()[i].label);
    return out;
  };

  const Graph fast_cand = spds::sliceGraphOnCandidatePaths(g, cand_pairs);
  const Graph ref_cand = refSlice(g, cand_pairs, false, {}, {});
  EXPECT_EQ(fast_cand.edges(), ref_cand.edges());

  const Graph fast_dem = spds::sliceGraphForDemands(
      g, demands.pairs, demands.sources, demands.targets);
  const Graph ref_dem = refSlice(g, cand_pairs, true, {}, {});
  EXPECT_EQ(fast_dem.edges(), ref_dem.edges());

  demands.sources = {3, 77};
  demands.targets = {19, 105};
  const Graph fast_dem_st = spds::sliceGraphForDemands(
      g, demands.pairs, demands.sources, demands.targets);
  const Graph ref_dem_st =
      refSlice(g, cand_pairs, true, demands.sources, demands.targets);
  EXPECT_EQ(fast_dem_st.edges(), ref_dem_st.edges());
}

TEST(InterleavedDyckHybridTest, LargeAlphabetGiantEpsilonSccCompletesSubsecond) {
  Graph g;
  constexpr Vertex N = 2000;
  for (Vertex v = 0; v < N; ++v)
    g.addEdge(v, (v + 1) % N, Label::neutral());
  for (unsigned lid = 0; lid < 3000; ++lid) {
    const Vertex u = static_cast<Vertex>(lid % N);
    const Vertex m = static_cast<Vertex>((lid * 7 + 3) % N);
    const Vertex w = static_cast<Vertex>((lid * 13 + 5) % N);
    g.addEdge(u, m, Label::openParenthesis(lid));
    g.addEdge(m, w, Label::closeParenthesis(lid));
  }
  Demands dem;
  for (Vertex i = 0; i < 100; ++i)
    dem.pairs.push_back({i * 10, (i * 10 + 999) % N});

  const auto started = std::chrono::steady_clock::now();
  const auto res = Solver{}.analyzeDemands(g, dem);
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started)
          .count();
  EXPECT_LT(elapsed_ms, 1000);
  EXPECT_EQ(res.certified_lower.size(), 100U);
  EXPECT_EQ(res.certified_lower, res.final_upper);
}

TEST(InterleavedDyckHybridTest,
     DualCoreSandwichDefaultAndG32CollisionElimination) {
  Graph g;
  const Vertex s = 0;
  const Vertex t = 99;
  // False positive (0, 99):
  //   Path A (call-valid, field-invalid): op1 ob0 cb8 cp1
  //     -> Under G=8, 0 % 8 == 8 % 8 == 0, so ob0 and cb8 collide to parity 0!
  //   Path B (field-valid, call-invalid): op1 op2 (ob0 cb0 ... ob8 cb8) cp1 cp2
  //     -> Supplies matched brackets 0..8 in corridor while violating call LIFO.
  g.addEdge(s, 1, Label::openParenthesis(1));
  g.addEdge(1, 2, Label::openBracket(0));
  g.addEdge(2, 3, Label::closeBracket(8));
  g.addEdge(3, t, Label::closeParenthesis(1));

  g.addEdge(s, 10, Label::openParenthesis(1));
  g.addEdge(10, 11, Label::openParenthesis(2));
  Vertex cur = 11;
  for (unsigned b = 0; b <= 8U; ++b) {
    const Vertex mid = cur + 1;
    const Vertex nxt = cur + 2;
    g.addEdge(cur, mid, Label::openBracket(b));
    g.addEdge(mid, nxt, Label::closeBracket(b));
    cur = nxt;
  }
  g.addEdge(cur, cur + 1, Label::closeParenthesis(1));
  g.addEdge(cur + 1, t, Label::closeParenthesis(2));

  // Genuine crossing witness (100, 104): op7 ob9 cp7 cb9
  g.addEdge(100, 101, Label::openParenthesis(7));
  g.addEdge(101, 102, Label::openBracket(9));
  g.addEdge(102, 103, Label::closeParenthesis(7));
  g.addEdge(103, 104, Label::closeBracket(9));

  Demands dem;
  dem.pairs = {{s, t}, {100, 104}};

  // With G=8, 0 and 8 wrap onto the same bucket when active rehash is disabled
  // (or even with active rehash when 9 types wrap into 8 buckets), check both
  // G=8 fixed modulo vs G=32 default best observer!
  Options g8;
  g8.parity_groups = 8U;
  g8.corridor_active_rehash = false;
  const auto res8 = Solver{}.analyzeDemands(g, dem, g8);
  EXPECT_EQ(res8.statistics.matrix_dimension, 16U);
  EXPECT_EQ(res8.stage1_spds.count({s, t}), 1U);
  EXPECT_EQ(res8.final_upper.count({s, t}), 1U);
  EXPECT_EQ(res8.final_upper.count({100, 104}), 1U);

  // Default Dual-Core Sandwich (G=32 + corridor_active_rehash=true) eliminates
  // the mod-8 collision cleanly, pruning (0, 99) while preserving (100, 104).
  const auto res32 = Solver{}.analyzeDemands(g, dem);
  EXPECT_EQ(res32.statistics.matrix_dimension, 64U);
  EXPECT_TRUE(res32.statistics.affine_executed);
  EXPECT_EQ(res32.stage1_spds.count({s, t}), 1U);
  EXPECT_EQ(res32.certified_lower.count({s, t}), 0U);
  EXPECT_EQ(res32.final_upper.count({s, t}), 0U);
  EXPECT_EQ(res32.final_upper.count({100, 104}), 1U);
}

} // namespace
} // namespace lotus::cfl::interleaved_dyck::hybrid
