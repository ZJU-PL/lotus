#pragma once

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalIFunc.h>
#include <llvm/IR/Instructions.h>

namespace lotus::concurrency {

inline const llvm::GlobalValue *getCalledGlobal(const llvm::CallBase *call) {
  if (!call)
    return nullptr;
  const llvm::Value *called = call->getCalledOperand();
  called = called ? called->stripPointerCasts() : nullptr;
  return llvm::dyn_cast_or_null<llvm::GlobalValue>(called);
}

inline const llvm::Function *resolveCallTarget(const llvm::CallBase *call) {
  if (!call)
    return nullptr;
  if (const llvm::Function *direct = call->getCalledFunction())
    return direct;

  const llvm::Value *current = call->getCalledOperand();
  llvm::SmallPtrSet<const llvm::Value *, 8> visited;
  while (current && visited.insert(current).second) {
    current = current->stripPointerCasts();
    if (const auto *function = llvm::dyn_cast<llvm::Function>(current))
      return function;
    if (const auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(current)) {
      current = alias->getAliaseeObject();
      continue;
    }
    if (const auto *ifunc = llvm::dyn_cast<llvm::GlobalIFunc>(current)) {
      const llvm::Function *resolver = ifunc->getResolverFunction();
      const llvm::Value *resolved_target = nullptr;
      if (!resolver || resolver->isDeclaration())
        return nullptr;
      for (const llvm::BasicBlock &block : *resolver) {
        const auto *ret =
            llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
        if (!ret || !ret->getReturnValue())
          continue;
        const llvm::Value *candidate =
            ret->getReturnValue()->stripPointerCasts();
        if (!resolved_target)
          resolved_target = candidate;
        else if (resolved_target != candidate)
          return nullptr;
      }
      current = resolved_target;
      continue;
    }
    break;
  }
  return nullptr;
}

} // namespace lotus::concurrency
