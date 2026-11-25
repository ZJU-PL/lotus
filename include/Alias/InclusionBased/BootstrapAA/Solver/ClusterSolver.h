#pragma once

#include "Alias/InclusionBased/BootstrapAA/IR/ProgramIndex.h"
#include "Alias/InclusionBased/BootstrapAA/Preprocessing/RelevantSlice.h"

#include <memory>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

// Abstract per-cluster solver interface shared by the faithful GuardedSummary
// backend (Algorithms 2-5) and the reference InputStateTabulation backend.
class ClusterSolver {
public:
  virtual ~ClusterSolver() = default;
  virtual QueryResult query(Id value, Id site, const Context *context_path,
                            Point point) = 0;
  virtual AliasResult queryAliases(Id value, Id site,
                                   const Context *context_path,
                                   Point point) = 0;
  virtual const Statistics &statistics() const = 0;
};

std::unique_ptr<ClusterSolver> createTabulationSolver(
    const Program &program, const ProgramIndex &index, const Sets &upper,
    const CallGraphInfo &call_graph, Slice slice, const Options &options);

std::unique_ptr<ClusterSolver> createGuardedSummarySolver(
    const Program &program, const ProgramIndex &index, const Sets &upper,
    const SteensgaardHierarchy &hierarchy, const CallGraphInfo &call_graph,
    const ReachabilityInfo &reachability, std::vector<Id> cluster_values,
    Slice slice, const Options &options);

} // namespace detail
} // namespace bootstrap
} // namespace lotus
