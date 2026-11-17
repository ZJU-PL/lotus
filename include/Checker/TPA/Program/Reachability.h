#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Checker/TPA/Support/ExternalModels.h"

#include <stdexcept>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace tpa::taint {
/// Resolve known targets without asserting on untracked pointers. Empty target
/// sets (including inline assembly) follow the same optimistic/strict policy
/// as missing external models. Resolved bodies and summaries are never skipped.
inline std::vector<const llvm::Function *>
resolveCallTargets(const tpa::SemiSparsePointerAnalysis &pointer,
                   const llvm::CallBase &call,
                   UnmodeledPolicy policy = UnmodeledPolicy::Ignore,
                   const context::Context *ctx = nullptr,
                   std::size_t *filteredTargets = nullptr) {
  std::vector<const llvm::Function *> targets;
  if (filteredTargets)
    *filteredTargets = 0;
  const auto *called = call.getCalledOperand()->stripPointerCasts();
  if (const auto *function = llvm::dyn_cast<llvm::Function>(called)) {
    targets.push_back(function);
  } else if (ctx || !pointer.getPointerManager()
                         .getPointersWithValue(called)
                         .empty()) {
    targets = pointer.getCallees(&call, ctx, filteredTargets);
  }
  if (targets.empty() && policy == UnmodeledPolicy::Error) {
    std::string description;
    llvm::raw_string_ostream out(description);
    out << "TPA taint cannot resolve call in " << call.getFunction()->getName()
        << ": " << call;
    throw std::invalid_argument(out.str());
  }
  return targets;
}

/// Functions reachable from main using TPA's merged call targets. This is only
/// for constructing the static sparse graph; propagation uses native contexts.
inline std::unordered_set<const llvm::Function *>
reachableFunctions(const llvm::Module &module,
                   const tpa::SemiSparsePointerAnalysis &pointer,
                   UnmodeledPolicy policy = UnmodeledPolicy::Ignore) {
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
        if (!call || isDebugIntrinsic(*call))
          continue;
        auto targets = resolveCallTargets(pointer, *call, policy);
        for (auto *target : targets)
          if (!target->isDeclaration() && reachable.insert(target).second)
            pending.push_back(target);
      }
  }
  return reachable;
}
} // namespace tpa::taint
