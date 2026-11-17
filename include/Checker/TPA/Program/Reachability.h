#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"

#include <stdexcept>
#include <unordered_set>

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace tpa::taint {
/// Functions reachable from main using TPA's merged call targets. This is only
/// for constructing the static sparse graph; propagation uses native contexts.
inline std::unordered_set<const llvm::Function *>
reachableFunctions(const llvm::Module &module,
                   const tpa::SemiSparsePointerAnalysis &pointer) {
  auto *main = module.getFunction("main");
  if (!main || main->isDeclaration())
    throw std::invalid_argument(
        "TPA taint requires a defined main entry point");
  std::unordered_set<const llvm::Function *> reachable{main};
  std::vector<const llvm::Function *> pending{main};
  while (!pending.empty()) {
    const auto *function = pending.back();
    pending.pop_back();
    for (const auto *block : llvm::depth_first(&function->getEntryBlock()))
      for (const auto &inst : *block) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!call)
          continue;
        if (!call->getCalledFunction() &&
            pointer.getPointerManager()
                .getPointersWithValue(
                    call->getCalledOperand()->stripPointerCasts())
                .empty())
          throw std::invalid_argument(
              "TPA taint cannot resolve indirect call in " +
              function->getName().str());
        auto targets = pointer.getCallees(call);
        if (targets.empty())
          throw std::invalid_argument("TPA taint has no call target in " +
                                      function->getName().str());
        for (auto *target : targets)
          if (!target->isDeclaration() && reachable.insert(target).second)
            pending.push_back(target);
      }
  }
  return reachable;
}
} // namespace tpa::taint
