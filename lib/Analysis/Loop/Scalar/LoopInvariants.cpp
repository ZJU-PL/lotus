/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Scalar/LoopInvariants.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"
#include "Analysis/Loop/Dependence/LoopDependenceBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {

bool isAllocatorLike(llvm::CallBase *call) {
  if (call == nullptr) {
    return false;
  }
  auto *callee = call->getCalledFunction();
  if (callee == nullptr || !callee->empty()) {
    return false;
  }

  auto &specManager = LoopDependenceBuilder::getAliasSpecManager();
  return specManager.isAllocator(callee) || specManager.isDeallocator(callee) ||
         specManager.getCategory(callee) ==
             lotus::alias::FunctionCategory::Reallocator;
}

bool isMemoryIndependentCall(llvm::CallBase *call) {
  if (call == nullptr) {
    return false;
  }
  return call->doesNotAccessMemory() && call->doesNotThrow() &&
         !call->isConvergent();
}

} // namespace

class InvariantManager::InvarianceChecker {
public:
  InvarianceChecker(LoopStructure *loop,
                    LoopDependenceGraph *loopDG,
                    std::unordered_set<llvm::Instruction *> &invariants)
      : loop{loop}, loopDG{loopDG}, invariants{invariants} {
    for (auto *inst : loop->getInstructions()) {
      if (inst->isTerminator()) {
        continue;
      }

      if (auto *call = llvm::dyn_cast<llvm::CallInst>(inst)) {
        if (isAllocatorLike(call)) {
          this->notInvariants.insert(inst);
          continue;
        }
      }

      bool isPHI = false;
      if (auto *phi = llvm::dyn_cast<llvm::PHINode>(inst)) {
        isPHI = true;
        if (!arePHIIncomingValuesEquivalent(phi)) {
          continue;
        }
      }

      if (this->notInvariants.count(inst) != 0) {
        continue;
      }

      this->dependencyValuesBeingChecked.clear();
      this->dependencyValuesBeingChecked.insert(inst);
      if (isPHI) {
        this->invariants.insert(inst);
      }

      auto canEvolve = this->loopDG->iterateOverDependencesTo(
          inst, false, true, true,
          [this](llvm::Value *toValue, LoopDependenceEdge *dep) {
            return this->isEvolvingValue(toValue, dep);
          });

      if (auto *call = llvm::dyn_cast<llvm::CallInst>(inst)) {
        auto *callee = call->getCalledFunction();
        if (callee != nullptr && callee->empty() &&
            !isMemoryIndependentCall(call)) {
          canEvolve = true;
        }
      }

      if (canEvolve) {
        this->invariants.erase(inst);
        this->notInvariants.insert(inst);
      } else {
        this->invariants.insert(inst);
      }
    }
  }

private:
  bool isEvolvingValue(llvm::Value *toValue, LoopDependenceEdge *dep) {
    auto *toInst = llvm::dyn_cast<llvm::Instruction>(toValue);
    if (toInst == nullptr || !this->loop->isIncluded(toInst)) {
      return false;
    }
    if (llvm::isa<llvm::StoreInst>(toInst)) {
      return true;
    }

    if (auto *call = llvm::dyn_cast<llvm::CallInst>(toInst)) {
      if (isAllocatorLike(call)) {
        return true;
      }
      if (isMemoryIndependentCall(call)) {
        return false;
      }
      auto *callee = call->getCalledFunction();
      if (callee != nullptr && callee->empty()) {
        return true;
      }
    }

    if (dep->getKind() == LoopDependenceEdgeKind::Memory) {
      return true;
    }

    bool isPHI = false;
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(toInst)) {
      isPHI = true;
      if (!arePHIIncomingValuesEquivalent(phi)) {
        return true;
      }
    }

    if (this->invariants.count(toInst) != 0) {
      return false;
    }
    if (this->notInvariants.count(toInst) != 0) {
      return true;
    }

    if (isPHI) {
      this->invariants.insert(toInst);
    }

    if (this->dependencyValuesBeingChecked.count(toInst) != 0) {
      return true;
    }
    this->dependencyValuesBeingChecked.insert(toInst);

    auto canEvolve = this->loopDG->iterateOverDependencesTo(
        toInst, false, true, true,
        [this](llvm::Value *nextValue, LoopDependenceEdge *nextDep) {
          return this->isEvolvingValue(nextValue, nextDep);
        });
    if (canEvolve) {
      this->invariants.erase(toInst);
      this->notInvariants.insert(toInst);
    } else {
      this->invariants.insert(toInst);
    }
    return canEvolve;
  }

  bool arePHIIncomingValuesEquivalent(llvm::PHINode *phi) const {
    std::unordered_set<llvm::Value *> incomingValues;
    for (auto &incomingUse : phi->incoming_values()) {
      incomingValues.insert(incomingUse.get());
    }
    if (incomingValues.empty()) {
      return false;
    }
    if (incomingValues.size() == 1) {
      return true;
    }

    llvm::GlobalValue *singleGlobalLoaded = nullptr;
    for (auto *incomingValue : incomingValues) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(incomingValue);
      if (load == nullptr) {
        return false;
      }
      auto *global =
          llvm::dyn_cast<llvm::GlobalValue>(load->getPointerOperand());
      if (global == nullptr) {
        return false;
      }
      if (singleGlobalLoaded == nullptr || singleGlobalLoaded == global) {
        singleGlobalLoaded = global;
        continue;
      }
      return false;
    }

    return singleGlobalLoaded != nullptr;
  }

  LoopStructure *loop;
  LoopDependenceGraph *loopDG;
  std::unordered_set<llvm::Instruction *> &invariants;
  std::unordered_set<llvm::Instruction *> notInvariants;
  std::unordered_set<llvm::Instruction *> dependencyValuesBeingChecked;
};

InvariantManager::InvariantManager(LoopStructure *loop,
                                   LoopDependenceGraph *loopDG)
    : loop{loop}, loopDG{loopDG} {
  for (auto *inst : loop->getInstructions()) {
    if (loop->isLoopInvariant(inst)) {
      this->invariants.insert(inst);
    }
  }

  InvarianceChecker checker{loop, loopDG, this->invariants};
  (void)checker;
}

bool InvariantManager::isLoopInvariant(llvm::Value *value) const {
  if (!llvm::isa<llvm::Instruction>(value)) {
    return true;
  }
  auto *inst = llvm::cast<llvm::Instruction>(value);
  if (!this->loop->isIncluded(inst)) {
    return true;
  }
  return this->invariants.count(inst) != 0;
}

bool InvariantManager::isLoopInvariant(LoopSCC *scc) const {
  auto interrupted = scc->iterateOverInstructions(
      [this](llvm::Instruction *inst) { return !this->isLoopInvariant(inst); });
  return !interrupted;
}

std::unordered_set<llvm::Instruction *>
InvariantManager::getLoopInstructionsThatAreLoopInvariants(void) const {
  return this->invariants;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
