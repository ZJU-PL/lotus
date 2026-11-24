/** @file PuritySummary.h @brief Purity summary data structures for function side-effect analysis. */
#pragma once

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace lotus {
namespace analysis {
namespace purity {

enum class PurityKind {
  Const = 0,
  Pure = 1,
  Impure = 2,
  Unknown = 3,
};

enum class SummarySource {
  InternalAnalysis = 0,
  LocalAttributes = 1,
  BuiltinSpec = 2,
  MemorySSA = 3,
  ExternalSummary = 4,
  Propagated = 5,
  ConservativeFallback = 6,
};

enum class SummaryConfidence {
  Low = 0,
  Medium = 1,
  High = 2,
};

llvm::StringRef toString(PurityKind kind);
llvm::StringRef toString(SummarySource source);
llvm::StringRef toString(SummaryConfidence confidence);

struct FunctionEffectSummary {
  bool readsReachableMemory = false;
  bool writesReachableMemory = false;
  bool hasObservableSideEffects = false;
  bool hasUnknownEffects = false;
  bool fromMemorySSA = false;
  SummarySource source = SummarySource::InternalAnalysis;
  SummaryConfidence confidence = SummaryConfidence::High;
  std::vector<std::string> dependsOn;

  PurityKind getPurityKind() const;
  bool isConstCandidate() const;
  bool isPureCandidate() const;

  void addDependency(llvm::StringRef symbol);
};

SummaryConfidence minConfidence(SummaryConfidence lhs,
                                SummaryConfidence rhs);

} // namespace purity
} // namespace analysis
} // namespace lotus
