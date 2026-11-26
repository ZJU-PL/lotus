#pragma once

#include "llvm/IR/Module.h"

#include "IR/PDG/Models/FindingTypes.h"

namespace pdg {
struct TaintFlowResult;

struct ArithmeticAnalysisResult {
  std::vector<AnalysisFinding> findings;
};

/// Numeric rules use mathematical intervals before machine-width truncation,
/// dominating CFG guards, and must-alias MemorySSA definitions. Taint is
/// supplied by PDG's own value flow service; unknown values are never taint
/// sources.
class ArithmeticAnalysis {
public:
  static const std::vector<AnalysisRuleDescriptor> &catalog();
  static bool requiresTaint(const std::string &id);
  ArithmeticAnalysisResult analyze(const llvm::Module &module,
                                const TaintFlowResult *taint = nullptr) const;
};
} // namespace pdg
