#pragma once

#include "IR/PDG/Models/FindingTypes.h"

namespace llvm {
class Module;
} // namespace llvm

namespace pdg {

struct ApiAnalysisResult {
  std::vector<AnalysisFinding> findings;
};

/// Library security protocols evaluated over LLVM values and executable CFG
/// paths. API spelling identifies a model; reporting requires its argument,
/// configuration state, or result-use precondition.
class ApiAnalysis {
public:
  static const std::vector<AnalysisRuleDescriptor> &catalog();
  ApiAnalysisResult analyze(const llvm::Module &module) const;
};

} // namespace pdg
