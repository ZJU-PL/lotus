#pragma once

#include "CFL/InterleavedDyck/SPDS/Solver.h"

namespace lotus::cfl::interleaved_dyck::spds {

// Balanced paths starting with an opening bracket and ending with a closing
// bracket. Results use original vertex IDs and exclude reflexive pairs, as in
// the staged-bounds value-flow client. Both projections remain upper bounds.
class ValueFlowAnalysis {
public:
  explicit ValueFlowAnalysis(const Graph &graph, Options options = {});
  Result analyzeAll(DemandDirection direction = DemandDirection::Auto) const;
  Result analyzeFrom(Vertex source) const;
  Result analyzeTo(Vertex target) const;
  Result
  analyzeDemands(const std::vector<Pair> &demands,
                 DemandDirection direction = DemandDirection::Auto) const;

private:
  void validate(Vertex vertex) const;
  Result restore(Result result) const;
  std::map<Vertex, Vertex> ids_;
  std::vector<Vertex> originals_, sources_, targets_;
  std::optional<PreparedAnalysis> analysis_;
};

} // namespace lotus::cfl::interleaved_dyck::spds
