#include "Analysis/Summary/Multiplicity/MultiplicityClassifier.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace {

using namespace lotus::analysis::multiplicity;

bool isHeapAllocationFunction(const llvm::Function *function,
                              const lotus::alias::AliasSpecManager &specs) {
  if (function == nullptr) {
    return false;
  }

  using lotus::alias::FunctionCategory;

  const auto category = specs.getCategory(function);
  return category == FunctionCategory::Allocator ||
         category == FunctionCategory::Reallocator;
}

bool isLoopBearingFunction(llvm::Function &function) {
  if (function.empty()) {
    return false;
  }

  llvm::DominatorTree dominator_tree(function);
  llvm::LoopInfo loop_info;
  loop_info.analyze(dominator_tree);
  return loop_info.begin() != loop_info.end();
}

llvm::DenseMap<const llvm::Function *, unsigned>
countDirectCallSites(llvm::Module &module) {
  llvm::DenseMap<const llvm::Function *, unsigned> call_site_counts;

  for (llvm::Function &function : module) {
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call == nullptr) {
        continue;
      }

      llvm::Function *callee = call->getCalledFunction();
      if (callee == nullptr) {
        continue;
      }

      ++call_site_counts[callee];
    }
  }

  return call_site_counts;
}

llvm::SmallPtrSet<const llvm::Function *, 16>
findLoopBearingFunctions(llvm::Module &module) {
  llvm::SmallPtrSet<const llvm::Function *, 16> functions_with_loops;

  for (llvm::Function &function : module) {
    if (isLoopBearingFunction(function)) {
      functions_with_loops.insert(&function);
    }
  }

  return functions_with_loops;
}

AllocationMultiplicity classifyAlloca(const llvm::AllocaInst &alloca,
                                      const llvm::SmallPtrSetImpl<
                                          const llvm::Function *> &
                                          functions_with_loops) {
  const llvm::Function *function = alloca.getFunction();
  if (function == nullptr || functions_with_loops.count(function) == 0) {
    return AllocationMultiplicity::Unique;
  }
  return AllocationMultiplicity::Summary;
}

AllocationMultiplicity classifyHeapAllocation(
    const llvm::CallInst &call,
    const llvm::SmallPtrSetImpl<const llvm::Function *> &functions_with_loops,
    const llvm::DenseMap<const llvm::Function *, unsigned> &call_site_counts) {
  const llvm::Function *function = call.getFunction();
  if (function == nullptr) {
    return AllocationMultiplicity::Summary;
  }

  if (functions_with_loops.count(function) != 0) {
    return AllocationMultiplicity::Summary;
  }

  auto it = call_site_counts.find(function);
  unsigned call_site_count = it == call_site_counts.end() ? 0 : it->second;
  if (call_site_count <= 1) {
    return AllocationMultiplicity::Unique;
  }

  return AllocationMultiplicity::Summary;
}

} // namespace

namespace lotus {
namespace analysis {
namespace multiplicity {

MultiplicityResult classifyModuleMultiplicity(llvm::Module &M) {
  MultiplicityResult result;
  lotus::alias::AliasSpecManager specs;
  specs.initialize(M);
  auto call_site_counts = countDirectCallSites(M);
  auto functions_with_loops = findLoopBearingFunctions(M);

  for (llvm::GlobalVariable &global : M.globals()) {
    result.allocations[&global] = AllocationMultiplicity::Unique;
  }

  for (llvm::Function &function : M) {
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
        result.allocations[alloca] = classifyAlloca(*alloca, functions_with_loops);
        continue;
      }

      auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      if (call == nullptr) {
        continue;
      }

      llvm::Function *callee = call->getCalledFunction();
      if (!isHeapAllocationFunction(callee, specs)) {
        continue;
      }

      auto *call_inst = llvm::dyn_cast<llvm::CallInst>(call);
      if (call_inst == nullptr) {
        result.allocations[call] = AllocationMultiplicity::Summary;
        continue;
      }

      result.allocations[call_inst] =
          classifyHeapAllocation(*call_inst, functions_with_loops, call_site_counts);
    }
  }

  return result;
}

} // namespace multiplicity
} // namespace analysis
} // namespace lotus
