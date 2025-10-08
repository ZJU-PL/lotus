#pragma once

#include <llvm/IR/PassManager.h>

namespace llvm {
class Module;
class OptimizationLevel;
}  // namespace llvm

namespace llvm_utils {

/// Run the default O0, O1, O2, or O3 optimization pass pipelines on the given module.
/// @param M The LLVM module to optimize (must not be null)
/// @param OptLevel The optimization level to apply
/// @return PreservedAnalyses indicating which analyses are preserved after optimization
llvm::PreservedAnalyses optimiseModule(llvm::Module *M,
                                       llvm::OptimizationLevel OptLevel);

}  // namespace llvm_utils

