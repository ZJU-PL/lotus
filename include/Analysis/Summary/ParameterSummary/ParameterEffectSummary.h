/** @file ParameterEffectSummary.h @brief Summary of parameter side effects for inter-procedural analysis. */
#pragma once

#include <llvm/ADT/DenseMap.h>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace lotus::analysis::parametersummary {

class ResourceTable;

struct ParameterEffectSummary {
  const llvm::Function *func = nullptr;
  llvm::DenseMap<unsigned, bool> paramFreed;
  llvm::DenseMap<unsigned, bool> paramDereferenced;
  bool returnIsAllocated = false;
};

using ParameterEffectSummaryMap =
    llvm::DenseMap<const llvm::Function *, ParameterEffectSummary>;

ParameterEffectSummaryMap computeParameterEffectSummaries(
    llvm::Module &M, const ResourceTable &table);
ParameterEffectSummaryMap computeParameterEffectSummaries(llvm::Module &M);

} // namespace lotus::analysis::parametersummary
