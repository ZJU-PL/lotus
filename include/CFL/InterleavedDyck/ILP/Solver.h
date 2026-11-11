#pragma once

#include "CFL/InterleavedDyck/Core/Graph.h"

#include <cstddef>

namespace lotus::cfl::interleaved_dyck::ilp {

/// Integer-linear feasibility backend used for each ILP disjunct.
/// Backend::Gurobi is available only in builds configured with
/// -DLOTUS_ENABLE_GUROBI=ON.
enum class Backend { Z3, Gurobi };

/// Single-source/single-target parenthesis-counting approximation from
/// Li, Zhang, and Reps, POPL 2023.
struct Options {
  /// Linear-programming backend. The default requires only Z3.
  Backend backend = Backend::Z3;
  /// Use the POPL 2017 LCL upper bound on each derived expression graph to
  /// discard subexpressions that cannot contain an InterDyck word.
  bool enable_lcl_refinement = true;
  /// Accept endpoint states with unmatched opens (the paper's optional
  /// nonnegative final-count constraints). The default is fully balanced.
  bool allow_unmatched_opens = false;
  /// Zero means unlimited. These limits prevent accidental exponential DNF
  /// expansion from exhausting the process.
  std::size_t max_disjuncts = 0;
  std::size_t max_expression_nodes = 0;
};

struct Statistics {
  std::size_t path_expression_nodes = 0;
  std::size_t ilp_disjuncts = 0;
  std::size_t rejected_bad_subexpressions = 0;
  std::size_t integer_variables = 0;
  std::size_t integer_constraints = 0;
};

struct Result {
  /// True means the paper's ILP over-approximation accepts the query. A
  /// positive result is not a concrete balanced-path witness.
  bool may_reach = false;
  Statistics statistics;
};

/// Returns whether the requested backend was compiled into this library.
/// The default Z3 backend is always available; Gurobi requires an explicit
/// optional build.
bool backendAvailable(Backend backend);

/// Demand-driven, directed, single-source/single-target InterDyck solver.
/// The input graph must be finite and completely materialized before query.
/// Invalid labels/options throw std::invalid_argument; configured resource
/// limits throw std::length_error; an unknown backend result throws
/// std::runtime_error.
class Solver {
public:
  Result analyze(const Graph &graph, Vertex source, Vertex target,
                 const Options &options = {}) const;
};

} // namespace lotus::cfl::interleaved_dyck::ilp
