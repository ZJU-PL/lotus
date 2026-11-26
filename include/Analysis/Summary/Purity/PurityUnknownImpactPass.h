/** @file PurityUnknownImpactPass.h @brief Pass for tagging functions with unknown purity impact. */
#pragma once

#include "llvm/Pass.h"

#include <memory>
#include <string>
#include <vector>

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace purity {

class DeclarationSummaryProvider;

struct UnknownCalleeImpact {
  std::string symbolName;
  unsigned directCallerCount = 0;
  unsigned transitiveCallerCount = 0;
};

struct PurityUnknownImpactPassOptions {
  std::vector<std::shared_ptr<const DeclarationSummaryProvider>>
      externalSummaryProviders;
};

class UnknownCalleeImpactAnalyzer {
public:
  explicit UnknownCalleeImpactAnalyzer(
      PurityUnknownImpactPassOptions options = {});

  std::vector<UnknownCalleeImpact> rankUnknownCallees(llvm::Module &module) const;

private:
  PurityUnknownImpactPassOptions options_;
};

class PurityUnknownImpactPass : public llvm::ModulePass {
public:
  static char ID;

  PurityUnknownImpactPass();
  explicit PurityUnknownImpactPass(PurityUnknownImpactPassOptions options);

  bool runOnModule(llvm::Module &module) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

private:
  PurityUnknownImpactPassOptions options_;
};

llvm::Pass *createPurityUnknownImpactPass(
    PurityUnknownImpactPassOptions options = {});

} // namespace purity
} // namespace analysis
} // namespace lotus
