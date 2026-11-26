/** @file FunctionPurityAnalysis.h @brief Function-level purity analysis. */
#pragma once

#include "Analysis/Summary/Purity/PuritySummary.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"

#include <memory>
#include <vector>

namespace lotus {
namespace analysis {
namespace purity {

enum class MemorySSAMode {
  Disabled = 0,
  UseIfAvailable = 1,
};

class DeclarationSummaryProvider;
class MemorySSAPuritySummaryProvider;

struct FunctionPurityAnalysisOptions {
  MemorySSAMode memorySSAMode = MemorySSAMode::UseIfAvailable;
  std::vector<std::shared_ptr<const DeclarationSummaryProvider>>
      declarationSummaryProviders;
  std::vector<std::shared_ptr<const DeclarationSummaryProvider>>
      externalSummaryProviders;
};

class FunctionPurityAnalysis {
public:
  explicit FunctionPurityAnalysis(
      llvm::Module &module,
      MemorySSAMode memorySSAMode = MemorySSAMode::UseIfAvailable);
  FunctionPurityAnalysis(llvm::Module &module,
                         FunctionPurityAnalysisOptions options);
  ~FunctionPurityAnalysis();

  void run();

  PurityKind getPurity(const llvm::Function *function) const;
  PurityKind getCallPurity(const llvm::CallBase &call) const;
  FunctionEffectSummary getEffects(const llvm::Function *function) const;

  bool hasMemorySSASummaries() const;

  bool isConst(const llvm::Function *function) const;
  bool isPure(const llvm::Function *function) const;
  bool isAtMostPure(const llvm::Function *function) const;
  bool isKnown(const llvm::Function *function) const;

private:
  llvm::Module &module_;
  FunctionPurityAnalysisOptions options_;
  llvm::DenseMap<const llvm::Function *, FunctionEffectSummary> summaries_;
  std::unique_ptr<MemorySSAPuritySummaryProvider> memorySSAProvider_;
  std::vector<std::shared_ptr<const DeclarationSummaryProvider>>
      declarationSummaryProviders_;
  bool ran_ = false;

  FunctionEffectSummary initialSummary(const llvm::Function &function) const;
  FunctionEffectSummary analyzeFunction(const llvm::Function &function) const;
  FunctionEffectSummary classifyDeclaration(
      const llvm::Function &function,
      const llvm::CallBase *callSite = nullptr) const;
  FunctionEffectSummary classifyCall(const llvm::CallBase &call) const;
  FunctionEffectSummary classifyIntrinsic(const llvm::Function &function) const;

  static FunctionEffectSummary merge(const FunctionEffectSummary &lhs,
                                     const FunctionEffectSummary &rhs);
};

} // namespace purity
} // namespace analysis
} // namespace lotus
