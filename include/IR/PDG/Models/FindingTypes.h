#pragma once

#include <string>
#include <vector>

namespace llvm {
class Instruction;
} // namespace llvm

namespace pdg {

/// Descriptor for a rule exposed by a PDG IR analysis.
struct AnalysisRuleDescriptor {
  std::string id;
  std::string severity;
  std::string codeql_query;
  std::string coverage;
};

/// Single finding emitted by a PDG IR analysis.
struct AnalysisFinding {
  std::string rule_id;
  const llvm::Instruction *site = nullptr;
  std::string message;
  std::vector<const llvm::Instruction *> evidence;
};

} // namespace pdg
