#pragma once

#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"
#include "CFL/InterleavedDyck/Core/Graph.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lotus::cfl::interleaved_dyck::hybrid {

struct Demands {
  std::vector<Pair> pairs;
  std::vector<Vertex> sources;
  std::vector<Vertex> targets;

  bool empty() const {
    return pairs.empty() && sources.empty() && targets.empty();
  }
};

struct Options {
  /// Number of 2x2 parity blocks per projection in Core 2's single-sided
  /// affine observer (default 32 -> ParitySemiring<uint32_t>, virtual 64-dim;
  /// active-rehash narrows to uint8_t/uint16_t automatically when <= 8 or <= 16
  /// matched brackets/parentheses are active in a corridor).
  unsigned parity_groups = 32;
  /// Rehash only active matched delimiter IDs inside each demand corridor and
  /// omit corridor-unmatched delimiters.
  bool corridor_active_rehash = true;
  /// Number of dead-edge feedback rounds after Core 2 (Stage 3) prunes
  /// candidates.
  unsigned affine_feedback_rounds = 1;

  /// Enable Stage 2a (demand union-Dyck L(G_1^+) lower bound inside Core 1).
  bool enable_lower = true;

  /// Enable Core 2 / Stage 3 (single-sided ParitySemiring AffineSPDS on
  /// unresolved U1 \ L1).
  bool enable_affine = true;
  /// Max pruned-corridor edges for Core 2 / Stage 3 (0 = unlimited; default
  /// 25000).
  std::size_t affine_max_edges = 25000;
  spds::Limits affine_limits;
};

struct Timings {
  std::uint64_t slice_us = 0;
  std::uint64_t stage1_spds_us = 0;
  std::uint64_t stage2_lower_us = 0;
  std::uint64_t stage3_affine_us = 0;
  std::uint64_t total_us = 0;
};

struct Statistics {
  std::size_t slice_vertices = 0;
  std::size_t slice_edges = 0;
  std::size_t post_stage3_vertices = 0;
  std::size_t post_stage3_edges = 0;
  std::size_t stage2_lower_count = 0;
  std::size_t matrix_dimension = 64;
  std::size_t max_affine_rank = 0;
  bool affine_executed = false;
  unsigned short_circuited_after = 0;
  Timings timings;
  affine::Statistics affine_stats;
};

struct Result {
  /// Stage 1: sound Boolean SPDS upper bound on demanded pairs.
  PairSet stage1_spds;
  /// Stage 2a: certified reachable pairs (union-Dyck lower bound).
  PairSet certified_lower;
  /// Stage 3: upper bound after single-sided ParitySemiring AffineSPDS +
  /// dead-edge feedback.
  PairSet stage3_affine;
  /// Final sound upper bound (= stage3_affine).
  PairSet final_upper;

  Statistics statistics;

  /// Unresolved gap pairs remaining between final_upper and certified_lower.
  std::size_t unresolvedCount() const {
    return final_upper.size() >= certified_lower.size()
               ? final_upper.size() - certified_lower.size()
               : 0U;
  }
};

/// Dual-Core Sandwich demand-driven synthesis pipeline:
///   Core 1 (DyckReachability Sandwich):
///     - Stage 1: Bit-parallel corridor slice + condensed shared-RHS Boolean
///       SPDS projection filter (U1 = D_P \cap D_B);
///     - Stage 2a: Condensed shared-RHS union-Dyck lower bound
///       (L1 = D_{P \cup B});
///   Core 2 (ParitySemiring AffineSPDS):
///     - Stage 3: Single-sided G-block parity observer (best observer default:
///       G=32, 64-dim virtual, corridor_active_rehash=true) on unresolved
///       (U1 \ L1) + dead-edge feedback.
///
/// Guarantees: certified_lower <= true_interdyck <= final_upper == stage3_affine
///             <= stage1_spds.
class Solver {
public:
  Result analyze(const Graph &graph, const Options &options = {}) const;
  Result analyzeDemands(const Graph &graph, const Demands &demands,
                        const Options &options = {}) const;
};

} // namespace lotus::cfl::interleaved_dyck::hybrid
