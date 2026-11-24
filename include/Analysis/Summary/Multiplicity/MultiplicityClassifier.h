/** @file MultiplicityClassifier.h @brief Classifier for multiplicity properties of program values. */
#pragma once

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace lotus {
namespace analysis {
namespace multiplicity {

enum class AllocationMultiplicity {
  Unique,
  Summary,
};

struct MultiplicityResult {
  llvm::DenseMap<const llvm::Value *, AllocationMultiplicity> allocations;
};

MultiplicityResult classifyModuleMultiplicity(llvm::Module &M);

} // namespace multiplicity
} // namespace analysis
} // namespace lotus
