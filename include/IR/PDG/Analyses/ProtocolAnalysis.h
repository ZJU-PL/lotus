#pragma once

#include "llvm/IR/Module.h"

#include "IR/PDG/Models/FindingTypes.h"

#include <cstddef>

namespace pdg {

struct ProtocolAnalysisResult {
  std::vector<AnalysisFinding> findings;
  bool incomplete = false;
};

/// Bounded, IR-visible protocol candidates. Thread launch overlap, concrete
/// shared objects and ordered CFG events are required; arbitrary dependence
/// reachability is never interpreted as a race or filesystem protocol.
class ProtocolAnalysis {
public:
  static const std::vector<AnalysisRuleDescriptor> &catalog();
  ProtocolAnalysisResult analyze(const llvm::Module &module,
                              size_t max_states = 4096) const;
};

} // namespace pdg
