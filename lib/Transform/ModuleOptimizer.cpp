// Module optimizer that runs LLVM's standard optimization pipelines.
// This provides a simple interface for applying O0, O1, O2, or O3 optimizations.

#include "Transform/ModuleOptimizer.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>

#include <stdexcept>

namespace llvm_utils {

using namespace llvm;

// Run the default O0, O1, O2, or O3 optimization pass pipelines on the given module
auto optimiseModule(Module *M, OptimizationLevel OptLevel) -> PreservedAnalyses {
  if (!M)
    throw std::invalid_argument("Null ptr argument!");

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  return PB.buildPerModuleDefaultPipeline(OptLevel).run(*M, MAM);
}

}  // namespace llvm_utils

