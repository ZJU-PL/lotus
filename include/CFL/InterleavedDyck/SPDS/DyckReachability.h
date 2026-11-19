#pragma once

#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/SPDS/Solver.h"

#include <vector>

namespace lotus::cfl::interleaved_dyck::spds {

enum class DyckProjectionKind { Parentheses, Brackets, UnionDyck };

/// Exact empty-stack Dyck-1 demand filtering using iterative Tarjan epsilon-SCC
/// contraction, hub-linearized midpoint collapse, and direction-adaptive shared
/// RHS summary tabulation.
PairSet filterDyckReachableDemands(const Graph &graph,
                                   const std::vector<Pair> &demands,
                                   DyckProjectionKind kind,
                                   bool include_self_pairs = false);

/// Exact empty-stack Dyck-1 single-anchor reachability (Post: anchor -> v,
/// Pre: v -> anchor). Always includes `{anchor}` if `anchor` is in `graph`.
std::vector<Vertex> queryDyckReachableFromAnchor(const Graph &graph,
                                                 Vertex anchor,
                                                 Direction direction,
                                                 DyckProjectionKind kind);

/// Interleaved-Dyck Boolean upper bound: evaluates the smaller-edge-count
/// single-stack Dyck-1 projection first, corridor-slices the graph to surviving
/// pairs, and evaluates the other projection on the sliced corridor.
PairSet filterInterleavedBooleanDemands(const Graph &graph,
                                        const std::vector<Pair> &demands,
                                        bool include_self_pairs = false);

/// All-pairs empty-stack Interleaved-Dyck Boolean upper bound.
PairSet filterInterleavedBooleanAll(const Graph &graph,
                                    bool include_self_pairs = false);

struct StrongComponents {
  std::uint32_t count = 0;
  std::vector<std::uint32_t> component_of;
};

/// Iterative Tarjan SCC decomposition over dense vertices `0 .. n-1`.
StrongComponents computeStrongComponents(
    std::uint32_t n, const std::vector<std::vector<std::uint32_t>> &adj);

/// Bit-parallel SCC-DAG corridor slicing for a set of candidate pairs.
Graph sliceGraphOnCandidatePaths(const Graph &graph, const PairSet &pairs);

/// Bit-parallel SCC-DAG demand slicing matching `staged_bounds::demandSlice`.
Graph sliceGraphForDemands(const Graph &graph, const std::vector<Pair> &pairs,
                           const std::vector<Vertex> &sources,
                           const std::vector<Vertex> &targets);

} // namespace lotus::cfl::interleaved_dyck::spds
