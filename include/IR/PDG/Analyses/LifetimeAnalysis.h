#pragma once

#include "IR/PDG/Models/FindingTypes.h"

#include <cstddef>

namespace llvm {
class Module;
} // namespace llvm

namespace pdg {

struct LifetimeAnalysisResult {
  std::vector<AnalysisFinding> findings;
  /// Objects whose exploration exceeded a state/call/count limit, encountered
  /// recursion, or re-entered their allocation site. No leak conclusion is
  /// emitted for these objects; witnessed local access/release bugs may remain.
  size_t incomplete_objects = 0;
  size_t budget_limited_objects = 0;
  bool state_limit_hit = false;
  size_t matched_calls = 0;
  size_t call_depth_limited_objects = 0;
  size_t summary_cache_hits = 0;
  size_t summary_cache_misses = 0;
  size_t lock_count_limited_objects = 0;
  size_t explored_states = 0;
};

/// PDG semantic support for allocation identity and resource lifecycle. Uses
/// LLVM CFG/SSA, matched direct calls, cached release/return summaries and
/// exact local stack locations directly, with no bug-engine, IFDS, or
/// graph-reachability-as-lifetime dependency. Alternatives retain separate
/// release state; overwrites kill local aliases. Unknown ownership effects
/// escape the object. The supported branch facts are Boolean identity and
/// allocation failure checks, not general SMT path feasibility.
class LifetimeAnalysis {
public:
  static const std::vector<AnalysisRuleDescriptor> &catalog();
  /// A zero budget permits unlimited exploration. Incomplete objects still
  /// report locally witnessed double frees/accesses, but never leak claims.
  LifetimeAnalysisResult analyze(const llvm::Module &module,
                              size_t max_states_per_object = 4096) const;
};

} // namespace pdg
