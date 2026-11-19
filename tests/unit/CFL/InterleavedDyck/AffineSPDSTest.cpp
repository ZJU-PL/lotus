#include "AffineSPDSTestSupport.h"
#include "CFL/InterleavedDyck/AffineSPDS/ParitySemiring.h"
#include "CFL/InterleavedDyck/SPDS/DyckReachability.h"

#include <gtest/gtest.h>
TEST(InterleavedDyckAffineSPDS, SharedSuites) {
  for (const auto &test : lotus::cfl::interleaved_dyck::affine::test::tests()) {
    SCOPED_TRACE(test.first);
    EXPECT_NO_THROW(test.second());
  }
}

namespace {
using namespace lotus::cfl::interleaved_dyck;
using namespace lotus::cfl::interleaved_dyck::affine;

TEST(InterleavedDyckAffineSPDS, IntersectionAndCertificateReuse) {
  auto point = Matrix::identity(14);
  auto a = AffineSpace::singleton(point);
  for (std::size_t i = 0; i < 128; ++i) {
    Matrix direction(14);
    direction.set(i / 14, i % 14);
    a.addPoint(point ^ direction);
  }
  auto counters = std::make_shared<AlgebraStatistics>();
  EXPECT_TRUE(a.intersects(a, counters.get()));
  EXPECT_EQ(counters->basis_reductions, 0u);
  EXPECT_EQ(counters->intersection_fast_paths, 1u);
  HistoryComparison comparison(AffineSpace::singleton(Matrix(2)),
                               AffineSpace::singleton(Matrix::identity(2)),
                               counters);
  EXPECT_FALSE(comparison.mayReach());
  const auto reductions = counters->basis_reductions;
  ASSERT_TRUE(comparison.certificate());
  EXPECT_TRUE(comparison.certificate()->verify(comparison.callHistory(),
                                               comparison.fieldHistory()));
  EXPECT_EQ(counters->basis_reductions, reductions);
}

TEST(InterleavedDyckAffineSPDS, DeltaStopsAtFullRankBeforeFallbackProducts) {
  auto counters = std::make_shared<AlgebraStatistics>();
  auto domain = AffineSemiring(2).withStatistics(counters);
  auto full = AffineSpace::top(2);
  auto target = AffineSpace::singleton(Matrix(2));
  for (std::size_t bit = 0; bit < 3; ++bit) {
    Matrix p(2);
    p.set(bit / 2, bit % 2);
    target.addPoint(p);
  }
  Matrix swap(2);
  swap.set(0, 1);
  swap.set(1, 0);
  const auto right = domain.lift(swap);
  AffineDelta input;
  input.pivots = {0, 1, 2, 3};
  AffineDelta output;
  EXPECT_TRUE(
      domain.extendDeltaAndCombine(target, full, right, input, true, &output));
  EXPECT_EQ(target.rank(), 4u);
  EXPECT_EQ(counters->matrix_products, 3u);
  input.full = true;
  const auto products = counters->matrix_products;
  EXPECT_FALSE(domain.extendPushDeltaAndCombine(target, right, full, full,
                                                input, true, &output));
  EXPECT_EQ(counters->matrix_products, products);
}

TEST(InterleavedDyckAffineSPDS, JointBlockCoordinatesMatchDenseAlgebra) {
  std::mt19937 rng(7301);
  for (const auto dimensions :
       {std::pair<std::size_t, std::size_t>{2, 3}, {9, 9}, {65, 2}}) {
    const auto n = dimensions.first + dimensions.second;
    auto layout = std::make_shared<const MatrixLayout>(
        n, std::vector<std::pair<std::size_t, std::size_t>>{
               {0, dimensions.first}, {dimensions.first, dimensions.second}});
    AffineSemiring compact(n, layout), dense(n);
    auto matrix = [&] {
      return Matrix::directSum({test::randomMatrix(dimensions.first, rng),
                                test::randomMatrix(dimensions.second, rng)});
    };
    for (unsigned trial = 0; trial < 6; ++trial) {
      auto a = compact.zero(), b = compact.zero(), ad = dense.zero(),
           bd = dense.zero();
      for (unsigned i = 0; i < 3; ++i) {
        const auto x = matrix(), y = matrix();
        a.addPoint(x);
        ad.addPoint(x);
        b.addPoint(y);
        bd.addPoint(y);
      }
      EXPECT_LT(a.coordinates(), n * n);
      EXPECT_EQ(a, ad);
      EXPECT_EQ(compact.extend(a, b), dense.extend(ad, bd));
      EXPECT_EQ(a.intersects(b), ad.intersects(bd));
      EXPECT_EQ(a.block(dimensions.first, dimensions.second),
                ad.block(dimensions.first, dimensions.second));
      const auto extra = matrix();
      auto incremental = compact.extend(a, b);
      AffineDelta delta, result_delta;
      auto grown = a;
      grown.joinWithDelta(compact.lift(extra), &delta);
      compact.extendDeltaAndCombine(incremental, grown, b, delta, true,
                                    &result_delta);
      EXPECT_EQ(incremental, compact.extend(grown, b));
      const auto fixed = compact.lift(matrix());
      const auto prepared = compact.prepareWeight(fixed);
      EXPECT_EQ(compact.extendPrepared(a, fixed, prepared),
                compact.extend(a, fixed));
    }
    auto left = compact.lift(Matrix(n));
    auto right = compact.lift(Matrix::identity(n));
    auto certificate = separate(left, right);
    ASSERT_TRUE(certificate);
    EXPECT_TRUE(certificate->verify(left, right));
    certificate->functional.set(0, dimensions.first);
    EXPECT_TRUE(certificate->verify(
        left, right)); // Off-block dual entries are irrelevant.
  }
}

TEST(InterleavedDyckAffineSPDS, CompressionPreservesCrossBlockCorrelations) {
  Graph graph;
  graph.addEdge(0, 1, Label::neutral());
  graph.addEdge(1, 2, Label::neutral());
  graph.addEdge(0, 2, Label::neutral());
  const auto observer =
      HistoryObserver::directSum({HistoryObserver::parity({graph.edges()[0]}),
                                  HistoryObserver::parity({graph.edges()[1]})});
  Options dense_options;
  dense_options.compress_observer = false;
  const auto compact = Solver{}.prepare(graph, observer);
  const auto dense = Solver(dense_options).prepare(graph, observer);
  auto c = compact.queryFrom(0), d = dense.queryFrom(0);
  EXPECT_EQ(c.statistics().coordinate_dimension, 8u);
  EXPECT_EQ(d.statistics().coordinate_dimension, 16u);
  EXPECT_EQ(c.compare(2).callHistory(), d.compare(2).callHistory());
  EXPECT_EQ(c.compare(2).callHistory().rank(), 1u);
  EXPECT_EQ(compact.analyzeAll().pairs, dense.analyzeAll().pairs);
  auto custom = observer;
  Matrix cross = observer.matrix(graph.edges()[0]);
  cross.set(0, 2);
  custom.set(graph.edges()[0], cross);
  EXPECT_FALSE(custom.compactLayout());
  EXPECT_EQ(Solver{}
                .prepare(graph, custom)
                .queryFrom(0)
                .statistics()
                .coordinate_dimension,
            16u);
}

TEST(InterleavedDyckAffineSPDS, SlicesReuseWeightsAndReadoutCaches) {
  Graph graph;
  graph.addEdge(0, 1, Label::neutral());
  graph.addEdge(1, 2, Label::neutral());
  graph.addEdge(2, 0, Label::neutral());
  graph.addEdge(100, 101, Label::neutral());
  const auto observer = HistoryObserver::parity({graph.edges()[0]});
  Options options;
  options.readout_batch_threshold = 1;
  auto analysis = Solver(options).prepare(graph, observer);
  auto first = analysis.queryFrom(0);
  EXPECT_EQ(first.statistics().prepared_weights, 2u);
  EXPECT_EQ(first.statistics().compiled_rules, 6u);
  auto second = analysis.queryFrom(1);
  EXPECT_EQ(second.statistics().slice_cache_hits, 1u);
  EXPECT_EQ(second.statistics().compiled_rules, 0u);
  EXPECT_EQ(second.statistics().prepared_weights, 0u);
  (void)first.compare(0);
  (void)first.compare(1);
  const auto products = first.statistics().algebra.matrix_products;
  (void)first.compare(2);
  EXPECT_EQ(first.statistics().algebra.matrix_products, products);
  const auto before_second = second.statistics().algebra.matrix_products;
  first.prepareReadout();
  EXPECT_EQ(second.statistics().algebra.matrix_products, before_second);
  EXPECT_LT(analysis.analyzeAll().statistics.saturation.rules,
            graph.vertices().size() * graph.edges().size() * 2);
  Options uncached;
  uncached.max_cached_slices = 0;
  EXPECT_EQ(analysis.analyzeAll().pairs,
            Solver(uncached).prepare(graph, observer).analyzeAll().pairs);
}

TEST(InterleavedDyckAffineSPDS, DemandCorridorAndObserverAllocationLimit) {
  Graph graph;
  graph.addEdge(0, 1, Label::neutral());
  graph.addEdge(1, 2, Label::neutral());
  for (Vertex v = 10; v < 100; ++v)
    graph.addEdge(v == 10 ? 0 : v - 1, v, Label::neutral());
  const auto observer = HistoryObserver::parity({graph.edges()[0]});
  const auto analysis = Solver{}.prepare(graph, observer);
  const auto demands = analysis.analyzeDemands({{0, 2}}, ComparisonMode::Joint,
                                               spds::DemandDirection::Post);
  EXPECT_TRUE(demands.mayReach(0, 2));
  EXPECT_EQ(demands.statistics.compiled_rules, 4u);
  Options options;
  options.max_matrix_dimension = 2;
  options.observer.max_events = std::numeric_limits<std::size_t>::max();
  options.observer.max_order_pairs = std::numeric_limits<std::size_t>::max();
  EXPECT_THROW(Solver(options).prepare(graph), spds::ResourceLimit);
  const auto identity =
      Solver{}.prepare(graph, HistoryObserver(32)).analyzeAll();
  EXPECT_EQ(identity.statistics.prepared_weights, 0u);
  EXPECT_EQ(identity.statistics.compiled_rules, 0u);
}

TEST(InterleavedDyckAffineSPDS, BooleanPreFilterShortCircuitsDemands) {
  // Anchor 0 has only close-parenthesis outgoing edges (unreenterable), while
  // downstream subgraph has cycles and non-identity observer matrices.
  Graph graph;
  graph.addEdge(0, 1, Label::closeParenthesis(0));
  graph.addEdge(1, 2, Label::openParenthesis(0));
  graph.addEdge(2, 1, Label::neutral());
  graph.addEdge(2, 3, Label::closeParenthesis(0));
  // Valid call path 4 -> 5 -> 6 -> 7, but invalid field path (missing cb--0).
  graph.addEdge(4, 5, Label::openParenthesis(1));
  graph.addEdge(5, 6, Label::openBracket(0));
  graph.addEdge(6, 7, Label::closeParenthesis(1));
  // Valid path 8 -> 9 -> 10, while non-identity edge sits in 11 -> 12.
  graph.addEdge(8, 9, Label::neutral());
  graph.addEdge(9, 10, Label::neutral());
  graph.addEdge(9, 11, Label::openParenthesis(2));
  graph.addEdge(11, 12, Label::neutral());

  HistoryObserver observer(2);
  Matrix toggle = Matrix::identity(2);
  toggle.set(0, 1);
  observer.set(graph.edges()[1], toggle);
  observer.set(graph.edges()[5], toggle);
  observer.set(graph.edges()[10], toggle);

  const auto analysis = Solver{}.prepare(graph, observer);

  // 1. Single-source / single-target anchor obstruction skips weighted saturation.
  const auto from0 = analysis.analyzeFrom(0);
  EXPECT_EQ(from0.pairs, (PairSet{{0, 0}}));
  EXPECT_EQ(from0.statistics.compiled_rules, 0u);
  EXPECT_EQ(from0.statistics.algebra.matrix_products, 0u);

  // Target 11 has only an op--2 incoming edge (unreenterable in Pre).
  const auto to11 = analysis.analyzeTo(11);
  EXPECT_EQ(to11.pairs, (PairSet{{11, 11}}));
  EXPECT_EQ(to11.statistics.compiled_rules, 0u);
  EXPECT_EQ(to11.statistics.algebra.matrix_products, 0u);

  // 2. Cross-projection mismatch (4 -> 7) skips weighted saturation.
  const auto pair47 = analysis.queryPair(4, 7);
  EXPECT_FALSE(pair47.mayReach(7));
  EXPECT_EQ(pair47.compare(7).verdict(), Verdict::ProjectionRejected);
  EXPECT_EQ(pair47.statistics().compiled_rules, 0u);
  EXPECT_EQ(pair47.statistics().algebra.matrix_products, 0u);
  EXPECT_FALSE(analysis.mayReach(4, 7, spds::DemandDirection::Pre));
  EXPECT_EQ(analysis.analyzeFrom(4).pairs, (PairSet{{4, 4}}));
  EXPECT_EQ(analysis.analyzeTo(7).pairs, (PairSet{{7, 7}}));

  // 3. Batch demands with only Boolean-unreachable pairs skip weighted saturation.
  const auto dead_batch =
      analysis.analyzeDemands({{0, 3}, {4, 7}, {8, 12}}, ComparisonMode::Joint);
  EXPECT_TRUE(dead_batch.pairs.empty());
  EXPECT_EQ(dead_batch.statistics.compiled_rules, 0u);
  EXPECT_EQ(dead_batch.statistics.algebra.matrix_products, 0u);

  // 4. Surviving corridor with only identity weights inside it skips saturation.
  const auto corridor810 = analysis.queryPair(8, 10);
  EXPECT_TRUE(corridor810.mayReach(10));
  EXPECT_EQ(corridor810.statistics().compiled_rules, 0u);
  EXPECT_EQ(corridor810.statistics().algebra.matrix_products, 0u);

  // 5. Projection mode skips weighted saturation even on non-identity corridors.
  const auto proj_only = analysis.analyzeDemands(
      {{8, 10}, {4, 7}}, ComparisonMode::Projection, spds::DemandDirection::Pre);
  EXPECT_EQ(proj_only.pairs, (PairSet{{8, 10}}));
  EXPECT_EQ(proj_only.statistics.compiled_rules, 0u);
  EXPECT_EQ(proj_only.statistics.algebra.matrix_products, 0u);

  // 6. Explicit stacks and prefix acceptance remain exact.
  const auto stack46 = analysis.queryPairStacks(4, 6, {1}, {0});
  EXPECT_TRUE(stack46.mayAccept(6, {1}, {0}));
  EXPECT_FALSE(analysis.queryPairStacks(4, 6, {2}, {0}).mayAccept(6, {2}, {0}));
  Options prefix_opts;
  prefix_opts.parentheses = spds::StackAcceptance::Any;
  prefix_opts.brackets = spds::StackAcceptance::Any;
  const auto prefix_analysis = Solver(prefix_opts).prepare(graph, observer);
  const auto prefix_demands = prefix_analysis.analyzeDemands(
      {{4, 6}, {5, 6}, {8, 12}}, ComparisonMode::Joint,
      spds::DemandDirection::Auto);
  EXPECT_TRUE(prefix_demands.mayReach(4, 6));
  EXPECT_TRUE(prefix_demands.mayReach(5, 6));
  EXPECT_TRUE(prefix_demands.mayReach(8, 12));
}

TEST(InterleavedDyckAffineSPDS, PreFilteredDemandsMatchFullSaturation) {
  std::mt19937 rng(202609);
  for (unsigned trial = 0; trial < 30; ++trial) {
    Graph g;
    for (Vertex v = 0; v < 6; ++v)
      g.addVertex(v);
    for (unsigned e = 0; e < 12; ++e)
      g.addEdge(rng() % 6, rng() % 6, test::label(rng() % 9));
    HistoryObserver observer(2);
    for (const auto &edge : g.edges())
      if (rng() % 2 == 0)
        observer.set(edge, test::randomMatrix(2, rng));
    for (bool prefix : {false, true}) {
      Options opts;
      if (prefix) {
        opts.parentheses = spds::StackAcceptance::Any;
        opts.brackets = spds::StackAcceptance::Any;
      }
      const auto analysis = Solver(opts).prepare(g, observer);
      std::vector<Pair> all_demands;
      for (Vertex s = 0; s < 6; ++s)
        for (Vertex t = 0; t < 6; ++t)
          all_demands.push_back({s, t});
      for (auto mode : {ComparisonMode::Joint, ComparisonMode::Independent,
                        ComparisonMode::Projection}) {
        const auto all = analysis.analyzeAll(mode);
        const auto batch_post =
            analysis.analyzeDemands(all_demands, mode, spds::DemandDirection::Post);
        EXPECT_EQ(batch_post.pairs, all.pairs);
        if (!prefix) {
          const auto batch_pre = analysis.analyzeDemands(
              all_demands, mode, spds::DemandDirection::Pre);
          EXPECT_EQ(batch_pre.pairs, all.pairs);
        } else {
          const auto batch_auto = analysis.analyzeDemands(
              all_demands, mode, spds::DemandDirection::Auto);
          EXPECT_EQ(batch_auto.pairs, all.pairs);
        }
        for (Vertex v = 0; v < 6; ++v) {
          const auto qf = analysis.queryFrom(v);
          const auto af = analysis.analyzeFrom(v, mode);
          for (Vertex u = 0; u < 6; ++u) {
            const bool expected =
                mode == ComparisonMode::Joint       ? qf.mayReach(u)
                : mode == ComparisonMode::Independent ? qf.independentMayReach(u)
                                                    : qf.spdsMayReach(u);
            EXPECT_EQ(af.mayReach(v, u), expected);
            EXPECT_EQ(all.mayReach(v, u), expected);
            if (mode == ComparisonMode::Joint) {
              EXPECT_EQ(analysis.mayReach(v, u, spds::DemandDirection::Post),
                        expected);
              if (!prefix)
                EXPECT_EQ(analysis.mayReach(v, u, spds::DemandDirection::Pre),
                          expected);
            }
          }
          if (!prefix) {
            const auto at = analysis.analyzeTo(v, mode);
            for (Vertex u = 0; u < 6; ++u)
              EXPECT_EQ(at.mayReach(u, v), all.mayReach(u, v));
          }
        }
      }
    }
  }
}

TEST(InterleavedDyckAffineSPDS,
     SingleSidedParity16DimMatchesSymmetric32DimExactly) {
  std::mt19937 rng(0x16D1A5);
  const Label palette[] = {
      Label::neutral(),           Label::openParenthesis(0),
      Label::closeParenthesis(0), Label::openParenthesis(3),
      Label::closeParenthesis(3), Label::openParenthesis(9),
      Label::closeParenthesis(9), Label::openBracket(0),
      Label::closeBracket(0),     Label::openBracket(2),
      Label::closeBracket(2),     Label::openBracket(8),
      Label::closeBracket(8),     Label::openBracket(11),
      Label::closeBracket(11)};
  for (unsigned trial = 0; trial < 40; ++trial) {
    Graph g;
    for (Vertex v = 0; v < 7; ++v)
      g.addVertex(v);
    for (unsigned e = 0; e < 18; ++e)
      g.addEdge(rng() % 7, rng() % 7, palette[rng() % 15]);
    std::vector<Pair> demands;
    for (Vertex s = 0; s < 7; ++s)
      for (Vertex t = 0; t < 7; ++t)
        demands.push_back({s, t});

    Solver solver;
    for (unsigned groups : {1U, 2U, 4U, 8U}) {
      const auto sym_obs = HistoryObserver::labelParity(g, groups);
      ASSERT_EQ(sym_obs.dimension(), static_cast<std::size_t>(4U * groups));
      const auto sym_prep = solver.prepare(g, sym_obs);

      for (auto mode : {ComparisonMode::Joint, ComparisonMode::Independent}) {
        SingleSidedParityOptions ss_opts;
        ss_opts.groups = groups;
        ss_opts.corridor_active_rehash = false;
        ss_opts.mode = mode;

        const auto sym_all = sym_prep.analyzeAll(mode);
        const auto ss_all = solver.analyzeSingleSidedParityAll(g, ss_opts);
        EXPECT_EQ(ss_all.statistics.matrix_dimension,
                  static_cast<std::size_t>(2U * groups));
        EXPECT_EQ(ss_all.pairs, sym_all.pairs);

        const auto sym_dem =
            sym_prep.analyzeDemands(demands, mode, spds::DemandDirection::Post);
        const auto ss_post = solver.analyzeSingleSidedParityDemands(
            g, demands, ss_opts, spds::DemandDirection::Post);
        const auto ss_pre = solver.analyzeSingleSidedParityDemands(
            g, demands, ss_opts, spds::DemandDirection::Pre);
        EXPECT_EQ(ss_post.pairs, sym_dem.pairs);
        EXPECT_EQ(ss_pre.pairs, sym_dem.pairs);
      }
    }
  }
}

TEST(InterleavedDyckAffineSPDS,
     SingleSidedCorridorRehashSeparatesModulo8Collision) {
  // Path 1 (call-valid, field-invalid): 0 --op0--> 1 --ob0--> 2 --cp0--> 3 --cb8--> 4
  // Path 2 (field-valid, call-invalid): 0 --op0--> 5 --ob0--> 6 --cp8--> 7 --cb0--> 4
  // Under fixed (id mod 8), 0 % 8 == 8 % 8 == 0, so ob0+cb8 and op0+cp8 both
  // cancel to zero parity! Under corridor_active_rehash, only {0, 8} are active
  // in the corridor, so 0 -> block 0 and 8 -> block 1 (4-dim <= 16-dim),
  // cleanly separating the false positive!
  Graph g;
  g.addEdge(0, 1, Label::openParenthesis(0));
  g.addEdge(1, 2, Label::openBracket(0));
  g.addEdge(2, 3, Label::closeParenthesis(0));
  g.addEdge(3, 4, Label::closeBracket(8));

  g.addEdge(0, 5, Label::openParenthesis(0));
  g.addEdge(5, 6, Label::openBracket(0));
  g.addEdge(6, 7, Label::closeParenthesis(8));
  g.addEdge(7, 4, Label::closeBracket(0));

  Solver solver;
  SingleSidedParityOptions fixed_opts{8U, false, ComparisonMode::Joint};
  SingleSidedParityOptions rehash_opts{8U, true, ComparisonMode::Joint};

  const auto fixed_res =
      solver.analyzeSingleSidedParityDemands(g, {{0, 4}}, fixed_opts);
  const auto rehash_res =
      solver.analyzeSingleSidedParityDemands(g, {{0, 4}}, rehash_opts);
  EXPECT_TRUE(fixed_res.mayReach(0, 4));
  EXPECT_FALSE(rehash_res.mayReach(0, 4));

  // Add a genuine InterDyck balanced path 0 -> 8 -> 9 -> 10 -> 4 and verify
  // corridor_active_rehash remains sound.
  g.addEdge(0, 8, Label::openParenthesis(0));
  g.addEdge(8, 9, Label::openBracket(8));
  g.addEdge(9, 10, Label::closeParenthesis(0));
  g.addEdge(10, 4, Label::closeBracket(8));
  const auto sound_res =
      solver.analyzeSingleSidedParityDemands(g, {{0, 4}}, rehash_opts);
  EXPECT_TRUE(sound_res.mayReach(0, 4));
}

TEST(InterleavedDyckAffineSPDS,
     SingleSidedMatchesSymmetricParityOnDenseNeutralCycles) {
  std::mt19937 rng(0x5CC917);
  const Label palette[] = {
      Label::neutral(),           Label::openParenthesis(0),
      Label::closeParenthesis(0), Label::openParenthesis(3),
      Label::closeParenthesis(3), Label::openBracket(0),
      Label::closeBracket(0),     Label::openBracket(8),
      Label::closeBracket(8)};
  for (unsigned trial = 0; trial < 30; ++trial) {
    Graph g;
    for (Vertex v = 0; v < 8; ++v)
      g.addVertex(v);
    for (Vertex v = 0; v < 8; ++v) {
      if (rng() % 2 == 0)
        g.addEdge(v, (v + 1) % 8, Label::neutral());
    }
    for (unsigned e = 0; e < 22; ++e)
      g.addEdge(rng() % 8, rng() % 8, palette[rng() % 9]);
    std::vector<Pair> demands;
    for (Vertex s = 0; s < 8; ++s)
      for (Vertex t = 0; t < 8; ++t)
        demands.push_back({s, t});

    Solver solver;
    const auto sym_obs = HistoryObserver::labelParity(g, 2U);
    const auto sym_prep = solver.prepare(g, sym_obs);
    for (auto mode : {ComparisonMode::Joint, ComparisonMode::Independent,
                      ComparisonMode::Projection}) {
      SingleSidedParityOptions ss_opts{2U, false, mode};
      const auto sym_dem =
          sym_prep.analyzeDemands(demands, mode, spds::DemandDirection::Post);
      const auto ss_post = solver.analyzeSingleSidedParityDemands(
          g, demands, ss_opts, spds::DemandDirection::Post);
      const auto ss_pre = solver.analyzeSingleSidedParityDemands(
          g, demands, ss_opts, spds::DemandDirection::Pre);
      EXPECT_EQ(ss_post.pairs, sym_dem.pairs);
      EXPECT_EQ(ss_pre.pairs, sym_dem.pairs);
    }
  }
}

template <class Word>
void verifyParityAlgebraIsomorphism(std::size_t bits, std::mt19937 &rng) {
  std::vector<std::pair<std::size_t, std::size_t>> blocks;
  blocks.reserve(bits);
  for (std::size_t b = 0; b < bits; ++b)
    blocks.emplace_back(2U * b, 2U);
  const std::size_t dim = 2U * bits;
  auto layout =
      bits >= 2U ? std::make_shared<const MatrixLayout>(dim, blocks) : nullptr;
  AffineSemiring dense_domain(dim, layout);
  AlgebraStatistics p_stats;
  ParitySemiring<Word> parity_domain(bits, &p_stats);

  auto toMatrix = [&](Word w) {
    Matrix m = Matrix::identity(dim);
    for (std::size_t b = 0; b < bits; ++b) {
      if (((static_cast<std::uint64_t>(w) >> b) & 1ULL) != 0ULL)
        m.set(2U * b, 2U * b + 1U);
    }
    return m;
  };
  auto randomWord = [&]() -> Word {
    const std::uint64_t raw =
        (static_cast<std::uint64_t>(rng()) << 32U) | rng();
    return static_cast<Word>(raw &
                             static_cast<std::uint64_t>(parity_domain.fullMask()));
  };

  EXPECT_EQ(parity_domain.zero(), parity_domain.zero());
  EXPECT_NE(parity_domain.zero(), parity_domain.one());
  EXPECT_TRUE(parity_domain.one().isIdentity());
  EXPECT_TRUE(parity_domain.containsZero(parity_domain.one()));

  // Verify self-aliasing in extendAndCombine (target == left, target == right,
  // target == left == right) and exact isomorphism with AffineSemiring.
  for (unsigned trial = 0; trial < 20; ++trial) {
    auto p1 = parity_domain.zero(), p2 = parity_domain.zero();
    auto d1 = dense_domain.zero(), d2 = dense_domain.zero();
    const unsigned pts1 = rng() % 5;
    const unsigned pts2 = rng() % 5;
    for (unsigned i = 0; i < pts1; ++i) {
      const Word w = randomWord();
      parity_domain.combineWith(p1, parity_domain.point(w));
      d1.addPoint(toMatrix(w));
    }
    for (unsigned i = 0; i < pts2; ++i) {
      const Word w = randomWord();
      parity_domain.combineWith(p2, parity_domain.point(w));
      d2.addPoint(toMatrix(w));
    }

    EXPECT_EQ(p1.rank(), d1.rank());
    EXPECT_EQ(p2.rank(), d2.rank());
    EXPECT_EQ(parity_domain.containsZero(p1),
              d1.intersects(dense_domain.one()));
    EXPECT_EQ(parity_domain.intersects(p1, p2), d1.intersects(d2));

    // Independent block check against blockwise AffineSpace intersection.
    bool expected_indep = !d1.empty() && !d2.empty();
    if (expected_indep) {
      for (std::size_t b = 0; b < bits; ++b) {
        if (!d1.block(2U * b, 2U).intersects(d2.block(2U * b, 2U))) {
          expected_indep = false;
          break;
        }
      }
    }
    EXPECT_EQ(parity_domain.independentIntersects(p1, p2), expected_indep);

    const auto p_prod = parity_domain.extend(p1, p2);
    const auto d_prod = dense_domain.extend(d1, d2);
    EXPECT_EQ(p_prod.rank(), d_prod.rank());
    EXPECT_EQ(parity_domain.containsZero(p_prod),
              d_prod.intersects(dense_domain.one()));

    // Self-aliasing checks for ReverseClose / SaturationSession in-place joins.
    auto p_alias_left = p1;
    auto p_alias_right = p2;
    auto p_alias_both = p1;
    auto d_alias_left = d1;
    auto d_alias_right = d2;
    auto d_alias_both = d1;
    const bool pc1 = parity_domain.extendAndCombine(p_alias_left, p_alias_left, p2);
    const bool dc1 = dense_domain.extendAndCombine(d_alias_left, d_alias_left, d2);
    EXPECT_EQ(pc1, dc1);
    EXPECT_EQ(p_alias_left.rank(), d_alias_left.rank());
    EXPECT_EQ(parity_domain.intersects(p_alias_left, p2),
              d_alias_left.intersects(d2));

    const bool pc2 = parity_domain.extendAndCombine(p_alias_right, p1, p_alias_right);
    const bool dc2 = dense_domain.extendAndCombine(d_alias_right, d1, d_alias_right);
    EXPECT_EQ(pc2, dc2);
    EXPECT_EQ(p_alias_right.rank(), d_alias_right.rank());

    const bool pc3 = parity_domain.extendAndCombine(p_alias_both, p_alias_both, p_alias_both);
    const bool dc3 = dense_domain.extendAndCombine(d_alias_both, d_alias_both, d_alias_both);
    EXPECT_EQ(pc3, dc3);
    EXPECT_EQ(p_alias_both.rank(), d_alias_both.rank());
  }

  // Full-rank saturation invariants: adding every standard basis toggle reaches
  // rank == bits and absorbs further insertions.
  auto full = parity_domain.one();
  for (std::size_t b = 0; b < bits; ++b) {
    EXPECT_TRUE(parity_domain.combineWith(full, parity_domain.toggle(b)));
  }
  EXPECT_EQ(full.rank(), bits);
  EXPECT_EQ(full.pivot_mask, parity_domain.fullMask());
  EXPECT_TRUE(parity_domain.containsZero(full));
  EXPECT_FALSE(parity_domain.combineWith(full, parity_domain.point(randomWord())));
  EXPECT_TRUE(parity_domain.intersects(full, parity_domain.point(randomWord())));
}

TEST(InterleavedDyckAffineSPDS, ParitySemiringAlgebraIsomorphismAndSelfAliasing) {
  std::mt19937 rng(0xB175A11U);
  verifyParityAlgebraIsomorphism<std::uint8_t>(1U, rng);
  verifyParityAlgebraIsomorphism<std::uint8_t>(7U, rng);
  verifyParityAlgebraIsomorphism<std::uint8_t>(8U, rng);
  verifyParityAlgebraIsomorphism<std::uint16_t>(9U, rng);
  verifyParityAlgebraIsomorphism<std::uint16_t>(16U, rng);
  verifyParityAlgebraIsomorphism<std::uint32_t>(17U, rng);
  verifyParityAlgebraIsomorphism<std::uint32_t>(32U, rng);
  verifyParityAlgebraIsomorphism<std::uint64_t>(33U, rng);
  verifyParityAlgebraIsomorphism<std::uint64_t>(64U, rng);
}

TEST(InterleavedDyckAffineSPDS,
     ParitySemiringMatchesDenseMatrixOracleAcrossWidthsAndModes) {
  std::mt19937 rng(0x9A817E55U);
  const Label palette[] = {
      Label::neutral(),            Label::openParenthesis(0),
      Label::closeParenthesis(0),  Label::openParenthesis(5),
      Label::closeParenthesis(5),  Label::openParenthesis(9),
      Label::closeParenthesis(9),  Label::openParenthesis(17),
      Label::closeParenthesis(17), Label::openBracket(0),
      Label::closeBracket(0),      Label::openBracket(3),
      Label::closeBracket(3),      Label::openBracket(8),
      Label::closeBracket(8),      Label::openBracket(16),
      Label::closeBracket(16)};
  for (unsigned trial = 0; trial < 12; ++trial) {
    Graph g;
    for (Vertex v = 0; v < 6; ++v)
      g.addVertex(v);
    for (Vertex v = 0; v < 6; ++v) {
      if ((rng() & 1U) == 0U)
        g.addEdge(v, (v + 1) % 6, Label::neutral());
    }
    for (unsigned e = 0; e < 18; ++e)
      g.addEdge(rng() % 6, rng() % 6, palette[rng() % 17]);

    std::vector<Pair> demands;
    for (Vertex s = 0; s < 6; ++s)
      for (Vertex t = 0; t < 6; ++t)
        demands.push_back({s, t});

    // Cover uint8_t (groups=1,4 -> 2,8 bits), uint16_t (groups=8 -> 16 bits),
    // and uint32_t (groups=9 -> 18 bits) against un-compressed dense
    // AffineSemiring matrix oracle.
    for (unsigned groups : {1U, 4U, 8U, 9U}) {
      const auto sym_obs = HistoryObserver::labelParity(g, groups);
      Options dense_opts;
      dense_opts.compress_observer = false;
      Options parity_opts;
      parity_opts.compress_observer = true;

      for (bool prefix : {false, true}) {
        dense_opts.parentheses = prefix ? spds::StackAcceptance::Any
                                        : spds::StackAcceptance::Empty;
        dense_opts.brackets = prefix ? spds::StackAcceptance::Any
                                     : spds::StackAcceptance::Empty;
        parity_opts.parentheses = dense_opts.parentheses;
        parity_opts.brackets = dense_opts.brackets;

        const auto dense_prep = Solver(dense_opts).prepare(g, sym_obs);
        const auto parity_prep = Solver(parity_opts).prepare(g, sym_obs);

        for (auto mode : {ComparisonMode::Joint, ComparisonMode::Independent,
                          ComparisonMode::Projection}) {
          EXPECT_EQ(parity_prep.analyzeAll(mode).pairs,
                    dense_prep.analyzeAll(mode).pairs);
          EXPECT_EQ(parity_prep.analyzeDemands(demands, mode,
                                               spds::DemandDirection::Post)
                        .pairs,
                    dense_prep.analyzeDemands(demands, mode,
                                              spds::DemandDirection::Post)
                        .pairs);
          if (!prefix) {
            EXPECT_EQ(parity_prep.analyzeDemands(demands, mode,
                                                 spds::DemandDirection::Pre)
                          .pairs,
                      dense_prep.analyzeDemands(demands, mode,
                                                spds::DemandDirection::Pre)
                          .pairs);

            if (mode != ComparisonMode::Projection) {
              SingleSidedParityOptions ss_opts{groups, false, mode};
              const auto ss_post = Solver{}.analyzeSingleSidedParityDemands(
                  g, demands, ss_opts, spds::DemandDirection::Post);
              const auto ss_pre = Solver{}.analyzeSingleSidedParityDemands(
                  g, demands, ss_opts, spds::DemandDirection::Pre);
              const auto expected = dense_prep.analyzeAll(mode).pairs;
              EXPECT_EQ(ss_post.pairs, expected);
              EXPECT_EQ(ss_pre.pairs, expected);
            }
          }
        }
      }
    }

    // Exercise uint16_t/uint32_t single-sided parity paths (groups = 9, 17).
    Solver ss_solver;
    const PairSet union_lb = spds::filterDyckReachableDemands(
        g, demands, spds::DyckProjectionKind::UnionDyck,
        /*include_self_pairs=*/true);
    for (unsigned groups : {9U, 17U}) {
      SingleSidedParityOptions ss_fixed{groups, false, ComparisonMode::Joint};
      SingleSidedParityOptions ss_rehash{groups, true, ComparisonMode::Joint};
      const auto p1 = ss_solver.analyzeSingleSidedParityDemands(
          g, demands, ss_fixed, spds::DemandDirection::Post);
      const auto p2 = ss_solver.analyzeSingleSidedParityDemands(
          g, demands, ss_fixed, spds::DemandDirection::Pre);
      EXPECT_EQ(p1.pairs, p2.pairs);

      // Single-pair corridors are symmetric in Post and Pre even with rehash.
      const Pair probe{0, 1};
      const auto r1 = ss_solver.analyzeSingleSidedParityDemands(
          g, {probe}, ss_rehash, spds::DemandDirection::Post);
      const auto r2 = ss_solver.analyzeSingleSidedParityDemands(
          g, {probe}, ss_rehash, spds::DemandDirection::Pre);
      EXPECT_EQ(r1.pairs, r2.pairs);

      // Multi-pair batch rehash depends on per-anchor corridors, so Post and Pre
      // may refine differently, while both remain sound upper bounds (>= LB).
      const auto br1 = ss_solver.analyzeSingleSidedParityDemands(
          g, demands, ss_rehash, spds::DemandDirection::Post);
      const auto br2 = ss_solver.analyzeSingleSidedParityDemands(
          g, demands, ss_rehash, spds::DemandDirection::Pre);
      for (const Pair &lb_pair : union_lb) {
        EXPECT_TRUE(br1.mayReach(lb_pair.source, lb_pair.target));
        EXPECT_TRUE(br2.mayReach(lb_pair.source, lb_pair.target));
      }
    }
  }
}

TEST(InterleavedDyckAffineSPDS, BestObserverDefaultEliminatesMod8Collision) {
  const SingleSidedParityOptions defaults;
  EXPECT_EQ(defaults.groups, 32U);
  EXPECT_TRUE(defaults.corridor_active_rehash);
  EXPECT_EQ(defaults.mode, ComparisonMode::Joint);

  Graph g;
  // False positive (0, 4): op0 ob0 cp0 cb8 vs. op0 ob0 cp8 cb0.
  g.addEdge(0, 1, Label::openParenthesis(0));
  g.addEdge(1, 2, Label::openBracket(0));
  g.addEdge(2, 3, Label::closeParenthesis(0));
  g.addEdge(3, 4, Label::closeBracket(8));

  g.addEdge(0, 5, Label::openParenthesis(0));
  g.addEdge(5, 6, Label::openBracket(0));
  g.addEdge(6, 7, Label::closeParenthesis(8));
  g.addEdge(7, 4, Label::closeBracket(0));

  // Genuine crossing witness (10, 14): op1 ob2 cp1 cb2.
  g.addEdge(10, 11, Label::openParenthesis(1));
  g.addEdge(11, 12, Label::openBracket(2));
  g.addEdge(12, 13, Label::closeParenthesis(1));
  g.addEdge(13, 14, Label::closeBracket(2));

  const auto res =
      Solver{}.analyzeSingleSidedParityDemands(g, {{0, 4}, {10, 14}});
  EXPECT_EQ(res.statistics.matrix_dimension, 64U);
  EXPECT_FALSE(res.mayReach(0, 4));
  EXPECT_TRUE(res.mayReach(10, 14));
}
} // namespace
