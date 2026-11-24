/** @file PurityAttrInferencePass.h @brief LLVM pass for inferring function purity attributes. */
#pragma once

#include "Analysis/Summary/Purity/FunctionPurityAnalysis.h"

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace purity {

class PurityAttrInferencePass : public llvm::ModulePass {
public:
  static char ID;

  PurityAttrInferencePass() : llvm::ModulePass(ID) {}

  bool runOnModule(llvm::Module &module) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

bool inferPurityAttributes(llvm::Module &module,
                           const FunctionPurityAnalysis &analysis);

llvm::Pass *createPurityAttrInferencePass();

} // namespace purity
} // namespace analysis
} // namespace lotus
