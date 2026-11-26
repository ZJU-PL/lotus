#include "Analysis/Summary/Purity/PuritySummary.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>

namespace lotus::analysis::purity {

llvm::StringRef toString(PurityKind kind) {
  switch (kind) {
  case PurityKind::Const:
    return "const";
  case PurityKind::Pure:
    return "pure";
  case PurityKind::Impure:
    return "impure";
  case PurityKind::Unknown:
    return "unknown";
  }
  llvm_unreachable("unknown PurityKind");
}

llvm::StringRef toString(SummarySource source) {
  switch (source) {
  case SummarySource::InternalAnalysis:
    return "internal-analysis";
  case SummarySource::LocalAttributes:
    return "local-attributes";
  case SummarySource::BuiltinSpec:
    return "builtin-spec";
  case SummarySource::MemorySSA:
    return "memoryssa";
  case SummarySource::ExternalSummary:
    return "external-summary";
  case SummarySource::Propagated:
    return "propagated";
  case SummarySource::ConservativeFallback:
    return "conservative-fallback";
  }
  llvm_unreachable("unknown SummarySource");
}

llvm::StringRef toString(SummaryConfidence confidence) {
  switch (confidence) {
  case SummaryConfidence::Low:
    return "low";
  case SummaryConfidence::Medium:
    return "medium";
  case SummaryConfidence::High:
    return "high";
  }
  llvm_unreachable("unknown SummaryConfidence");
}

PurityKind FunctionEffectSummary::getPurityKind() const {
  if (writesReachableMemory || hasObservableSideEffects) {
    return PurityKind::Impure;
  }
  if (hasUnknownEffects) {
    return PurityKind::Unknown;
  }
  if (readsReachableMemory) {
    return PurityKind::Pure;
  }
  return PurityKind::Const;
}

bool FunctionEffectSummary::isConstCandidate() const {
  return getPurityKind() == PurityKind::Const;
}

bool FunctionEffectSummary::isPureCandidate() const {
  const PurityKind kind = getPurityKind();
  return kind == PurityKind::Const || kind == PurityKind::Pure;
}

void FunctionEffectSummary::addDependency(llvm::StringRef symbol) {
  if (symbol.empty()) {
    return;
  }

  const auto it = std::lower_bound(dependsOn.begin(), dependsOn.end(), symbol);
  if (it != dependsOn.end() && *it == symbol) {
    return;
  }
  dependsOn.insert(it, symbol.str());
}

SummaryConfidence minConfidence(SummaryConfidence lhs,
                                SummaryConfidence rhs) {
  return static_cast<int>(lhs) < static_cast<int>(rhs) ? lhs : rhs;
}

} // namespace lotus::analysis::purity
