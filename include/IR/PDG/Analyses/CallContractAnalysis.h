#pragma once

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Models/FindingTypes.h"

namespace pdg {

struct CallContractAnalysisResult {
  std::vector<AnalysisFinding> findings;
};

/// Contracts for promoted format arguments, resolved C call signatures, and
/// module-wide variadic sentinel conventions. Unsupported format directives or
/// unresolved callees remain unknown instead of guessing a source-level type.
class CallContractAnalysis {
public:
  static const std::vector<AnalysisRuleDescriptor> &catalog();
  CallContractAnalysisResult analyze(const llvm::Module &module) const;
};

} // namespace pdg
