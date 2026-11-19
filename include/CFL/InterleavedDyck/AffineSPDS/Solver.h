#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Affine.h"
#include "CFL/InterleavedDyck/AffineSPDS/Observer.h"
#include "CFL/InterleavedDyck/SPDS/Solver.h"

#include <memory>

namespace lotus::cfl::interleaved_dyck::affine {

enum class Verdict { MayReach, ProjectionRejected, AffineSeparated };
enum class ComparisonMode { Joint, Independent, Projection };

class HistoryComparison {
public:
  HistoryComparison(AffineSpace call_history, AffineSpace field_history,
                    std::shared_ptr<AlgebraStatistics> statistics = nullptr);
  bool mayReach() const;
  bool spdsMayReach() const { return !calls_.empty() && !fields_.empty(); }
  Verdict verdict() const;
  const AffineSpace &callHistory() const { return calls_; }
  const AffineSpace &fieldHistory() const { return fields_; }
  const std::optional<SeparationCertificate> &certificate() const;
  // Ablation: forget cross-block relationships AFTER computing the histories.
  // On block-diagonal observers this equals separate per-block analyses.
  bool independentMayReach(const HistoryObserver &observer) const;

private:
  AffineSpace calls_, fields_;
  mutable std::optional<Verdict> verdict_;
  mutable std::optional<std::optional<SeparationCertificate>> certificate_;
  std::shared_ptr<AlgebraStatistics> statistics_;
  mutable std::optional<
      std::pair<std::vector<std::pair<std::size_t, std::size_t>>, bool>>
      independent_;
};

// Generic use with paired affine-weighted PDS automata. Both systems must use
// the SAME event interpretation; this is the caller's semantic responsibility.
HistoryComparison synchronize(const spds::Automaton<AffineSemiring> &calls,
                              const spds::Configuration &call_query,
                              const spds::Automaton<AffineSemiring> &fields,
                              const spds::Configuration &field_query);

struct Options {
  ObserverOptions observer;
  spds::StackAcceptance parentheses = spds::StackAcceptance::Empty;
  spds::StackAcceptance brackets = spds::StackAcceptance::Empty;
  spds::Limits
      limits; // per projection; failures throw, never return partial bounds
  std::size_t max_matrix_dimension = 0; // 0 = no explicit dimension cap
  bool compress_observer = true;
  // 0 disables automatic promotion; full-endpoint APIs still batch readout.
  std::size_t readout_batch_threshold = 8;
  // Compiled slices are cached by SCC and direction. Either zero disables
  // caching.
  std::size_t max_cached_slices = 8;
  std::size_t max_cached_slice_rules = 200000;
};
struct Statistics {
  spds::Statistics saturation;
  std::size_t matrix_dimension = 1;
  std::size_t maximum_affine_rank = 0;
  std::size_t coordinate_dimension = 1;
  std::uint64_t observer_microseconds = 0;
  std::size_t slice_cache_hits = 0;
  std::size_t compiled_rules = 0;
  std::size_t prepared_weights = 0;
  AlgebraStatistics algebra;
};
struct Result {
  PairSet pairs;
  ComparisonMode mode = ComparisonMode::Joint;
  Statistics statistics;
  bool mayReach(Vertex source, Vertex target) const {
    return pairs.count({source, target}) != 0;
  }
};

class QueryResult {
public:
  Vertex anchor() const { return anchor_; }
  spds::Direction direction() const { return direction_; }
  const HistoryObserver &observer() const { return *observer_; }
  const spds::Automaton<AffineSemiring> &callAutomaton() const {
    return calls_;
  }
  const spds::Automaton<AffineSemiring> &fieldAutomaton() const {
    return fields_;
  }
  Statistics statistics() const;
  void prepareReadout() const;
  // Lazy cache: const query methods are not safe for concurrent use of ONE
  // QueryResult. Distinct results/solvers share no mutable global state.
  const HistoryComparison &compare(Vertex vertex) const;
  bool mayReach(Vertex vertex) const { return compare(vertex).mayReach(); }
  bool spdsMayReach(Vertex vertex) const {
    return compare(vertex).spdsMayReach();
  }
  bool independentMayReach(Vertex vertex) const {
    return compare(vertex).independentMayReach(observer());
  }
  bool parenthesisReachable(Vertex vertex) const {
    return !compare(vertex).callHistory().empty();
  }
  bool bracketReachable(Vertex vertex) const {
    return !compare(vertex).fieldHistory().empty();
  }
  // Post: exact endpoint stacks; Pre: exact predecessor stacks. Top first,
  // ordinary unsigned label IDs; callers do not add the protected bottom.
  HistoryComparison compareStacks(Vertex vertex,
                                  const std::vector<unsigned> &calls,
                                  const std::vector<unsigned> &fields) const;
  bool mayAccept(Vertex vertex, const std::vector<unsigned> &calls,
                 const std::vector<unsigned> &fields) const {
    return compareStacks(vertex, calls, fields).mayReach();
  }

private:
  friend class PreparedAnalysis;
  QueryResult(Vertex anchor, spds::Direction direction, Options options,
              std::shared_ptr<const std::map<Vertex, spds::State>> controls,
              std::shared_ptr<const HistoryObserver> observer,
              spds::Automaton<AffineSemiring> calls,
              spds::Automaton<AffineSemiring> fields);
  AffineSpace history(const spds::Automaton<AffineSemiring> &automaton,
                      Vertex vertex, spds::StackAcceptance acceptance) const;
  Vertex anchor_;
  spds::Direction direction_;
  Options options_;
  std::shared_ptr<const std::map<Vertex, spds::State>> controls_;
  std::shared_ptr<const HistoryObserver> observer_;
  spds::Automaton<AffineSemiring> calls_, fields_;
  mutable Statistics statistics_;
  mutable std::map<Vertex, HistoryComparison> cache_;
  mutable std::optional<HistoryComparison> missing_;
  mutable std::optional<std::vector<AffineSpace>> call_weights_, field_weights_;
  using StackKey =
      std::tuple<Vertex, std::vector<unsigned>, std::vector<unsigned>>;
  mutable std::map<StackKey, HistoryComparison> stack_cache_;
};

struct SingleSidedParityOptions {
  // Number of 2x2 parity blocks per projection; the default best observer sets
  // groups = 32 (64-dim virtual observer backed by ParitySemiring<uint32_t>)
  // with corridor-local active rehashing (automatically narrowing to uint8_t or
  // uint16_t when <= 8 or <= 16 matched delimiters are active in a corridor).
  unsigned groups = 32;
  // When true (default), drops corridor-unmatched delimiters and rehashes only
  // active matched delimiter IDs in each demand corridor, eliminating fixed
  // modulo collisions when active delimiter types <= groups.
  bool corridor_active_rehash = true;
  ComparisonMode mode = ComparisonMode::Joint;
};

// Fixed computational algebra, no SAT/observer synthesis. Computes exact
// affine hulls for EACH projected witness language under the given observer.
// Their intersection is a sound upper bound, NOT a concrete two-stack witness.
class PreparedAnalysis;
class Solver {
public:
  explicit Solver(Options options = {}) : options_(options) {}
  PreparedAnalysis prepare(const Graph &graph) const;
  PreparedAnalysis prepare(const Graph &graph,
                           const HistoryObserver &observer) const;

  Result analyzeSingleSidedParityAll(
      const Graph &graph, SingleSidedParityOptions parity = {}) const;
  Result analyzeSingleSidedParityFrom(
      const Graph &graph, Vertex source,
      SingleSidedParityOptions parity = {}) const;
  Result analyzeSingleSidedParityTo(
      const Graph &graph, Vertex target,
      SingleSidedParityOptions parity = {}) const;
  Result analyzeSingleSidedParityDemands(
      const Graph &graph, const std::vector<Pair> &demands,
      SingleSidedParityOptions parity = {},
      spds::DemandDirection direction = spds::DemandDirection::Auto) const;

private:
  void validate(const Graph &graph, const HistoryObserver &observer) const;
  Options options_;
};

class PreparedAnalysis {
public:
  QueryResult queryFrom(Vertex source) const;
  QueryResult queryTo(Vertex target) const;
  QueryResult queryPair(
      Vertex source, Vertex target,
      spds::DemandDirection direction = spds::DemandDirection::Auto,
      ComparisonMode mode = ComparisonMode::Joint) const;
  QueryResult queryPairStacks(
      Vertex source, Vertex target, const std::vector<unsigned> &calls,
      const std::vector<unsigned> &fields,
      spds::DemandDirection direction = spds::DemandDirection::Auto,
      ComparisonMode mode = ComparisonMode::Joint) const;
  HistoryComparison compare(
      Vertex source, Vertex target,
      spds::DemandDirection direction = spds::DemandDirection::Auto) const;
  bool mayReach(
      Vertex source, Vertex target,
      spds::DemandDirection direction = spds::DemandDirection::Auto) const {
    return compare(source, target, direction).mayReach();
  }
  Result analyzeFrom(Vertex source,
                     ComparisonMode mode = ComparisonMode::Joint) const;
  Result analyzeTo(Vertex target,
                   ComparisonMode mode = ComparisonMode::Joint) const;
  Result analyzeAll(ComparisonMode mode = ComparisonMode::Joint) const;
  Result analyzeDemands(
      const std::vector<Pair> &demands,
      ComparisonMode mode = ComparisonMode::Joint,
      spds::DemandDirection direction = spds::DemandDirection::Auto) const;
  const HistoryObserver &observer() const;

private:
  friend class Solver;
  struct Storage;
  explicit PreparedAnalysis(std::shared_ptr<Storage> storage);
  QueryResult query(Vertex anchor, spds::Direction direction,
                    const std::vector<Vertex> *endpoints = nullptr) const;
  QueryResult queryPairImpl(Vertex source, Vertex target,
                            const std::vector<unsigned> *calls,
                            const std::vector<unsigned> *fields,
                            spds::DemandDirection direction,
                            ComparisonMode mode) const;
  Result analyzeAnchor(Vertex anchor, spds::Direction direction,
                       ComparisonMode mode) const;
  Result analyzeSingleSidedAnchor(Vertex anchor, spds::Direction direction,
                                  SingleSidedParityOptions parity) const;
  std::shared_ptr<Storage> storage_;
};

} // namespace lotus::cfl::interleaved_dyck::affine
