#pragma once

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Models/FindingTypes.h"

#include <map>

namespace pdg {

enum class PointerNullState { Unknown, Null, NonNull, Nullable };
enum class InitializationState {
  Unknown,
  Uninitialized,
  Initialized,
  MaybeUninitialized
};

struct PointerStateFact {
  const llvm::Value *pointer = nullptr;
  PointerNullState nullness = PointerNullState::Unknown;
  InitializationState pointee_initialization = InitializationState::Unknown;
};

struct StateAnalysisResult {
  std::vector<AnalysisFinding> findings;
  /// Facts immediately before pointer operands are used. A missing observation
  /// is unknown, not a proof of initialization or nonnullness.
  std::map<const llvm::Instruction *, std::vector<PointerStateFact>> states;
  size_t analyzed_functions = 0;
  bool convergence_limit_hit = false;

  PointerStateFact stateBefore(const llvm::Instruction &instruction,
                               const llvm::Value &pointer) const;
};

/// PDG-native intraprocedural state service over the retained LLVM CFG.
/// Tracks scalar stack cells and exact conditional output memory regions,
/// strong stores, predecessor joins, stable value snapshots, signed constant
/// Guards, and pointer-null refinements. Scanf effects relate assignment counts
/// to output initialization; bounded direct-call summaries relate integral
/// return statuses to scalar/field writes. Unknown writes and escaping cells
/// become unknown. Mixed uninitialized paths require a bounded Guard-consistent
/// witness; dependence reachability alone is never definite initialization.
class StateAnalysis {
public:
  static const std::vector<AnalysisRuleDescriptor> &catalog();
  StateAnalysisResult analyze(const llvm::Module &module,
                           const std::vector<std::string> &rule_ids = {}) const;
};

} // namespace pdg
