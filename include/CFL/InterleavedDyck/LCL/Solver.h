#pragma once

#include "CFL/InterleavedDyck/Core/Graph.h"

#include <cstddef>

namespace lotus::cfl::interleaved_dyck::lcl {

/// Zhang and Su, POPL 2017, Algorithms 1 and 2, instantiated with Table 2.
enum class Algorithm { Baseline, Refined };

struct Options {
  Algorithm algorithm = Algorithm::Refined;
  /// Section 5.3 endpoint filters. Ignored by Algorithm::Baseline.
  bool enable_feasibility = true;
  /// Optional hard limits; zero means unlimited. Exceeding a limit throws
  /// std::length_error, never returns a partial (and unsound) upper bound.
  std::size_t max_summaries = 0;
  std::size_t max_normalized_edges = 0;
};

struct Statistics {
  std::size_t input_vertices = 0;
  std::size_t input_edges = 0;
  std::size_t normalized_edges = 0;
  std::size_t epsilon_pairs = 0;
  std::size_t summaries = 0;
  std::size_t gray_summaries = 0;
  std::size_t summary_upgrades = 0;
  std::size_t left_term_updates = 0;
  std::size_t right_term_updates = 0;
  std::size_t rule_lookups = 0;
  std::size_t worklist_pops = 0;
  std::size_t peak_worklist = 0;
};

struct Result {
  /// Sound upper bound: presence does NOT certify a balanced witness path.
  /// Absence proves unreachability in the supplied directed graph.
  /// Includes empty paths at every vertex and all neutral-only paths.
  PairSet upper_bound;
  /// Pairs for which the LCL saturation constructed at least one trellis
  /// summary, regardless of whether that summary is accepting. This is useful
  /// when composing path-expression fragments with unmatched boundary labels.
  PairSet summary_pairs;
  Statistics statistics;

  /// Expected O(1). Vertices absent from the input return false.
  bool mayReach(Vertex source, Vertex target) const {
    return upper_bound.count({source, target}) != 0U;
  }
};

/// All-pairs linear-conjunctive-language approximation for two typed Dyck
/// alphabets. Uses Core's directed graph without symmetrization or unary
/// projection. Neutral edges denote epsilon and are eliminated exactly before
/// saturation. No stack-depth or path-length bound is used.
///
/// Refined mode implements Table 2 and Algorithm 2 with monotone white/gray
/// summary facts. Boundary-label masks retain all feasible derivations on
/// multigraphs, independently of insertion/worklist order (see module README).
/// Baseline mode implements Algorithm 1's white-summary saturation.
///
/// For a fixed alphabet and an epsilon-free graph, expected saturation time is
/// O(|V|*|E|) and space O(|V|^2), apart from storing the input. Hash tables give
/// expected, not worst-case, constant-time membership. With neutral edges the
/// bound applies to the epsilon-eliminated graph, which can be denser.
class Solver {
public:
  /// Stateless/reentrant; graph is not modified. Invalid labels/options throw
  /// std::invalid_argument. Resource exhaustion is reported by an exception.
  Result analyze(const Graph &graph, const Options &options = {}) const;
};

} // namespace lotus::cfl::interleaved_dyck::lcl
