/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Memory/MemoryCloningAnalysis.h"

#include "Analysis/CFG/Dominator.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <cassert>
#include <queue>

namespace lotus {
namespace analysis {
namespace loop {

llvm::AllocaInst *ClonableMemoryObject::getAllocation(void) const {
  return this->allocation;
}

uint64_t ClonableMemoryObject::getAllocationSizeInBits(void) const {
  return this->sizeInBits;
}

bool ClonableMemoryObject::mustAliasAMemoryLocationWithinObject(
    llvm::Value *pointer) const {
  if (pointer == this->allocation) {
    return true;
  }
  return this->castsAndGEPs.count(
             llvm::dyn_cast_or_null<llvm::Instruction>(pointer)) != 0;
}

bool ClonableMemoryObject::isInstructionCastOrGEPOfLocation(
    llvm::Instruction *inst) const {
  return this->castsAndGEPs.count(inst) != 0;
}

bool ClonableMemoryObject::isInstructionStoringLocation(
    llvm::Instruction *inst) const {
  return this->storingInstructions.count(inst) != 0;
}

bool ClonableMemoryObject::isInstructionLoadingLocation(
    llvm::Instruction *inst) const {
  return this->nonStoringInstructions.count(inst) != 0 ||
         this->loadInstructions.count(inst) != 0;
}

bool ClonableMemoryObject::isInstructionUsingLocationWithoutStoring(
    llvm::Instruction *inst) const {
  return this->nonStoringInstructions.count(inst) != 0;
}

void ClonableMemoryObject::addPointer(llvm::Instruction *inst) {
  this->castsAndGEPs.insert(inst);
}

void ClonableMemoryObject::addStore(llvm::Instruction *inst) {
  this->storingInstructions.insert(inst);
}

void ClonableMemoryObject::addLoad(llvm::Instruction *inst) {
  this->loadInstructions.insert(inst);
}

void ClonableMemoryObject::addNonStoringUse(llvm::Instruction *inst) {
  this->nonStoringInstructions.insert(inst);
}

std::unordered_set<llvm::Instruction *>
ClonableMemoryObject::getLocationPointerInstructions(void) const {
  return this->castsAndGEPs;
}

void ClonableMemoryObject::setClonable(bool clonable) {
  this->clonable = clonable;
}

void ClonableMemoryObject::setNeedsInitialization(bool needsInitialization) {
  this->needsInitialization = needsInitialization;
}

void ClonableMemoryObject::setObjectScope(llvm::AllocaInst *allocation,
                                          LoopStructure *loop,
                                          const noelle::DominatorSummary &DS) {
  std::vector<llvm::Instruction *> lifetimeStarts;
  std::vector<llvm::Instruction *> lifetimeEnds;
  for (auto *inst : loop->getInstructions()) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(inst);
    if (call == nullptr || !call->isLifetimeStartOrEnd()) {
      continue;
    }
    auto *objectUsed = call->getArgOperand(1)->stripPointerCasts();
    if (objectUsed != allocation) {
      continue;
    }
    auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call);
    if (intrinsic == nullptr) {
      continue;
    }
    if (intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_start) {
      lifetimeStarts.push_back(call);
    } else if (intrinsic->getIntrinsicID() == llvm::Intrinsic::lifetime_end) {
      lifetimeEnds.push_back(call);
    }
  }

  std::unordered_set<llvm::Instruction *> objectUses;
  objectUses.insert(this->storingInstructions.begin(),
                    this->storingInstructions.end());
  objectUses.insert(this->loadInstructions.begin(),
                    this->loadInstructions.end());
  objectUses.insert(this->nonStoringInstructions.begin(),
                    this->nonStoringInstructions.end());
  if (objectUses.empty()) {
    return;
  }

  for (auto *start : lifetimeStarts) {
    for (auto *end : lifetimeEnds) {
      if (!DS.DT.dominates(start, end)) {
        continue;
      }
      bool containsEveryUse = true;
      for (auto *use : objectUses) {
        if (!loop->isIncluded(use) || !DS.DT.dominates(start, use) ||
            !DS.PDT.dominates(end, use)) {
          containsEveryUse = false;
          break;
        }
      }
      if (containsEveryUse) {
        this->scopeWithinLoop = true;
        return;
      }
    }
  }
}

bool ClonableMemoryObject::isMemCpyInstrinsicCall(llvm::CallInst *call) {
  if (call == nullptr) {
    return false;
  }
  auto *calledFn = call->getCalledFunction();
  return calledFn != nullptr && calledFn->hasName() &&
         calledFn->getName().contains("llvm.memcpy");
}

bool ClonableMemoryObject::identifyStoresAndOtherUsers(
    LoopStructure *loop,
    const noelle::DominatorSummary &DS) {
  std::queue<llvm::Instruction *> allocationUses;
  allocationUses.push(this->allocation);
  while (!allocationUses.empty()) {
    auto *I = allocationUses.front();
    allocationUses.pop();

    for (auto *user : I->users()) {
      if (auto *cast = llvm::dyn_cast<llvm::CastInst>(user)) {
        allocationUses.push(cast);
        this->castsAndGEPs.insert(cast);
        continue;
      }
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
        allocationUses.push(gep);
        this->castsAndGEPs.insert(gep);
        continue;
      }

      auto *inst = llvm::dyn_cast<llvm::Instruction>(user);
      if (inst == nullptr) {
        return false;
      }

      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        if (store->getPointerOperand() != I) {
          return false;
        }
        this->storingInstructions.insert(store);
      } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
        if (load->getPointerOperand() != I) {
          return false;
        }
        this->loadInstructions.insert(load);
      } else if (auto *call = llvm::dyn_cast<llvm::CallInst>(inst)) {
        if (call->isLifetimeStartOrEnd()) {
          continue;
        }
        bool isMemCpy = isMemCpyInstrinsicCall(call);
        bool isDestinationUse =
            call->arg_size() >= 3 && call->getArgOperand(0) == I;
        bool isSourceUse = call->arg_size() >= 3 && call->getArgOperand(1) == I;
        if (!isMemCpy || (!isDestinationUse && !isSourceUse)) {
          return false;
        }
        if (isDestinationUse) {
          this->storingInstructions.insert(call);
        }
        if (isSourceUse) {
          this->loadInstructions.insert(call);
        }
      } else {
        return false;
      }

      if (!loop->isIncluded(inst) &&
          !DS.DT.dominates(inst->getParent(), loop->getHeader())) {
        return false;
      }

      if (llvm::isa<llvm::InvokeInst>(inst)) {
        return false;
      }
    }
  }

  return true;
}

bool ClonableMemoryObject::isThereAMemoryDependenceBetweenLoopIterations(
    LoopStructure *loop,
    llvm::AllocaInst *,
    LoopDependenceGraph *ldg,
    const std::unordered_set<llvm::Instruction *> &insts) const {
  for (auto *inst : insts) {
    if (!loop->isIncluded(inst)) {
      continue;
    }
    auto functor = [loop](llvm::Value *fromValue, LoopDependenceEdge *d) {
      auto *otherInst = llvm::dyn_cast<llvm::Instruction>(fromValue);
      return otherInst != nullptr && loop->isIncluded(otherInst) &&
             d->isLoopCarried();
    };

    if (ldg->iterateOverDependencesTo(inst, false, false, true, functor) ||
        ldg->iterateOverDependencesFrom(inst, false, false, true, functor)) {
      return true;
    }
  }

  return false;
}

bool ClonableMemoryObject::isThereRAWThroughMemoryBetweenLoopIterations(
    LoopStructure *loop,
    llvm::AllocaInst *allocation,
    LoopDependenceGraph *ldg) const {
  return this->isThereRAWThroughMemoryBetweenLoopIterations(
      loop, allocation, ldg, this->loadInstructions);
}

bool ClonableMemoryObject::isThereRAWThroughMemoryBetweenLoopIterations(
    LoopStructure *loop,
    llvm::AllocaInst *,
    LoopDependenceGraph *ldg,
    const std::unordered_set<llvm::Instruction *> &insts) const {
  for (auto *inst : insts) {
    if (!loop->isIncluded(inst)) {
      continue;
    }
    auto functor = [loop](llvm::Value *fromValue, LoopDependenceEdge *d) {
      auto *otherInst = llvm::dyn_cast<llvm::Instruction>(fromValue);
      return otherInst != nullptr && loop->isIncluded(otherInst) &&
             d->getKind() == LoopDependenceEdgeKind::Memory &&
             d->getMemoryKind() == LoopDependenceMemoryKind::Raw &&
             d->isLoopCarried();
    };
    if (ldg->iterateOverDependencesTo(inst, false, false, true, functor)) {
      return true;
    }
  }
  return false;
}

bool ClonableMemoryObject::isThereRAWThroughMemoryFromOutsideToLoop(
    LoopStructure *loop,
    llvm::AllocaInst *allocation,
    LoopDependenceGraph *ldg) const {
  return this->isThereRAWThroughMemoryFromOutsideToLoop(
             loop, allocation, ldg, this->loadInstructions) ||
         this->isThereRAWThroughMemoryFromOutsideToLoop(
             loop, allocation, ldg, this->nonStoringInstructions);
}

bool ClonableMemoryObject::isThereRAWThroughMemoryFromOutsideToLoop(
    LoopStructure *loop,
    llvm::AllocaInst *,
    LoopDependenceGraph *ldg,
    const std::unordered_set<llvm::Instruction *> &insts) const {
  for (auto *inst : insts) {
    if (!loop->isIncluded(inst)) {
      continue;
    }
    auto functor = [loop](llvm::Value *fromValue, LoopDependenceEdge *d) {
      auto *otherInst = llvm::dyn_cast<llvm::Instruction>(fromValue);
      return otherInst != nullptr && !loop->isIncluded(otherInst) &&
             d->getKind() == LoopDependenceEdgeKind::Memory &&
             d->getMemoryKind() == LoopDependenceMemoryKind::Raw;
    };
    if (ldg->iterateOverDependencesTo(inst, false, false, true, functor)) {
      return true;
    }
  }
  return false;
}

bool ClonableMemoryObject::isThereRAWThroughMemoryFromLoopToOutside(
    LoopStructure *loop,
    llvm::AllocaInst *allocation,
    LoopDependenceGraph *ldg) const {
  return this->isThereRAWThroughMemoryFromLoopToOutside(
             loop, allocation, ldg, this->storingInstructions) ||
         this->isThereRAWThroughMemoryFromLoopToOutside(
             loop, allocation, ldg, this->nonStoringInstructions);
}

bool ClonableMemoryObject::isThereRAWThroughMemoryFromLoopToOutside(
    LoopStructure *loop,
    llvm::AllocaInst *,
    LoopDependenceGraph *ldg,
    const std::unordered_set<llvm::Instruction *> &insts) const {
  for (auto *inst : insts) {
    if (!loop->isIncluded(inst)) {
      continue;
    }
    auto functor = [loop](llvm::Value *toValue, LoopDependenceEdge *d) {
      auto *otherInst = llvm::dyn_cast<llvm::Instruction>(toValue);
      return otherInst != nullptr && !loop->isIncluded(otherInst) &&
             d->getKind() == LoopDependenceEdgeKind::Memory &&
             d->getMemoryKind() == LoopDependenceMemoryKind::Raw;
    };
    if (ldg->iterateOverDependencesFrom(inst, false, false, true, functor)) {
      return true;
    }
  }
  return false;
}

bool ClonableMemoryObject::identifyInitialStoringInstructions(
    LoopStructure *loop,
    const noelle::DominatorSummary &DS) {
  std::unordered_set<llvm::Instruction *> instructionsNeedingCoverage;
  instructionsNeedingCoverage.insert(this->nonStoringInstructions.begin(),
                                     this->nonStoringInstructions.end());
  instructionsNeedingCoverage.insert(this->loadInstructions.begin(),
                                     this->loadInstructions.end());

  for (auto *instToCover : instructionsNeedingCoverage) {
    bool belongsToExistingSet = false;
    for (auto &overrideSet : this->overrideSets) {
      auto *dominatingBlock = overrideSet->dominatingBlockOfNonStoringInsts;
      if (DS.DT.dominates(dominatingBlock, instToCover->getParent())) {
        overrideSet->subsequentNonStoringInstructions.insert(instToCover);
        belongsToExistingSet = true;
        break;
      }
    }
    if (belongsToExistingSet) {
      continue;
    }
    auto overrideSet = std::make_unique<OverrideSet>();
    overrideSet->dominatingBlockOfNonStoringInsts = instToCover->getParent();
    overrideSet->subsequentNonStoringInstructions.insert(instToCover);
    this->overrideSets.push_back(std::move(overrideSet));
  }

  for (auto *storingInstruction : this->storingInstructions) {
    if (!loop->isIncluded(storingInstruction)) {
      continue;
    }
    for (auto &overrideSet : this->overrideSets) {
      bool dominatesEveryUse = true;
      for (auto *use : overrideSet->subsequentNonStoringInstructions) {
        if (!DS.DT.dominates(storingInstruction, use)) {
          dominatesEveryUse = false;
          break;
        }
      }
      if (dominatesEveryUse) {
        overrideSet->initialStoringInstructions.insert(storingInstruction);
      }
    }
  }

  for (const auto &overrideSet : this->overrideSets) {
    if (overrideSet->initialStoringInstructions.empty()) {
      return false;
    }
  }

  return true;
}

bool ClonableMemoryObject::areOverrideSetsFullyCoveringTheAllocationSpace(
    void) const {
  if (this->overrideSets.empty()) {
    return false;
  }
  for (const auto &overrideSet : this->overrideSets) {
    if (!this->isOverrideSetFullyCoveringTheAllocationSpace(overrideSet.get())) {
      return false;
    }
  }
  return true;
}

bool ClonableMemoryObject::isOverrideSetFullyCoveringTheAllocationSpace(
    OverrideSet *overrideSet) const {
  if (this->sizeInBits == 0 || this->sizeInBits % 8 != 0) {
    return false;
  }

  const auto &DL = this->allocation->getModule()->getDataLayout();
  uint64_t allocationSize = this->sizeInBits / 8;
  std::vector<std::pair<uint64_t, uint64_t>> writtenRanges;
  auto addWrittenRange = [&](llvm::Value *pointer, uint64_t bytes) {
    int64_t offset = 0;
    auto *base = llvm::GetPointerBaseWithConstantOffset(pointer, offset, DL);
    if (base != this->allocation || offset < 0 || bytes == 0) {
      return;
    }
    auto start = static_cast<uint64_t>(offset);
    if (start > allocationSize || bytes > allocationSize - start) {
      return;
    }
    writtenRanges.emplace_back(start, start + bytes);
  };

  for (auto *storingInstruction : overrideSet->initialStoringInstructions) {
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(storingInstruction)) {
      auto storeSize = DL.getTypeStoreSize(store->getValueOperand()->getType());
      if (storeSize.isScalable()) {
        continue;
      }
      addWrittenRange(store->getPointerOperand(), storeSize.getFixedSize());
    } else if (auto *call = llvm::dyn_cast<llvm::CallInst>(storingInstruction)) {
      if (!isMemCpyInstrinsicCall(call)) {
        continue;
      }
      auto *bytesStoredConst =
          llvm::dyn_cast<llvm::ConstantInt>(call->getOperand(2));
      if (bytesStoredConst == nullptr) {
        continue;
      }
      addWrittenRange(call->getArgOperand(0), bytesStoredConst->getZExtValue());
    }
  }

  if (writtenRanges.empty()) {
    return false;
  }
  std::sort(writtenRanges.begin(), writtenRanges.end());
  uint64_t coveredUntil = 0;
  for (const auto &range : writtenRanges) {
    if (range.first > coveredUntil) {
      return false;
    }
    coveredUntil = std::max(coveredUntil, range.second);
    if (coveredUntil == allocationSize) {
      return true;
    }
  }
  return false;
}

ClonableMemoryObject::ClonableMemoryObject(llvm::AllocaInst *allocation,
                                           uint64_t sizeInBits,
                                           LoopStructure *loop,
                                           const noelle::DominatorSummary &DS,
                                           LoopDependenceGraph *ldg)
    : allocation{allocation}, sizeInBits{sizeInBits}, loop{loop} {
  this->allocatedType = allocation->getAllocatedType();
  if (!this->identifyStoresAndOtherUsers(loop, DS)) {
    return;
  }
  this->setObjectScope(allocation, loop, DS);

  if (!this->isThereAMemoryDependenceBetweenLoopIterations(
          loop, allocation, ldg, this->storingInstructions) &&
      !this->isThereAMemoryDependenceBetweenLoopIterations(
          loop, allocation, ldg, this->nonStoringInstructions) &&
      !this->scopeWithinLoop) {
    return;
  }

  if (this->isThereRAWThroughMemoryBetweenLoopIterations(loop, allocation, ldg)) {
    return;
  }

  if (this->scopeWithinLoop) {
    this->clonable = true;
    return;
  }

  if (!this->isThereRAWThroughMemoryFromLoopToOutside(loop, allocation, ldg)) {
    if (!this->isThereRAWThroughMemoryFromOutsideToLoop(loop, allocation, ldg)) {
      this->clonable = true;
      return;
    }

    this->needsInitialization = true;
    this->clonable = true;
    return;
  }

  if (!this->allocatedType->isStructTy() &&
      !this->allocatedType->isIntegerTy()) {
    return;
  }

  this->identifyInitialStoringInstructions(loop, DS);
  if (!this->scopeWithinLoop) {
    if (!this->areOverrideSetsFullyCoveringTheAllocationSpace() ||
        this->isThereRAWThroughMemoryFromLoopToOutside(loop, allocation, ldg)) {
      return;
    }
  }

  this->clonable = true;
}

bool ClonableMemoryObject::isClonableLocation(void) const {
  return this->clonable;
}

bool ClonableMemoryObject::doPrivateCopiesNeedToBeInitialized(void) const {
  return this->needsInitialization;
}

MemoryCloningAnalysis::MemoryCloningAnalysis(
    LoopStructure *loop,
    const noelle::DominatorSummary &DS,
    LoopDependenceGraph *ldg) {
  assert(loop != nullptr);
  assert(ldg != nullptr);

  auto *function = loop->getFunction();
  auto &entryBlock = function->getEntryBlock();
  const auto &DL = function->getParent()->getDataLayout();

  for (auto &I : entryBlock) {
    auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
    if (alloca == nullptr) {
      continue;
    }
    auto sizeInBitsOptional = alloca->getAllocationSizeInBits(DL);
    if (!sizeInBitsOptional.hasValue()) {
      continue;
    }

    auto location = std::make_unique<ClonableMemoryObject>(
        alloca, sizeInBitsOptional.getValue(), loop, DS, ldg);
    if (!location->isClonableLocation()) {
      continue;
    }
    this->clonableMemoryLocations.push_back(std::move(location));
  }
}

std::unordered_set<ClonableMemoryObject *>
MemoryCloningAnalysis::getClonableMemoryObjects(void) const {
  std::unordered_set<ClonableMemoryObject *> locations;
  for (const auto &location : this->clonableMemoryLocations) {
    locations.insert(location.get());
  }
  return locations;
}

std::unordered_set<ClonableMemoryObject *>
MemoryCloningAnalysis::getClonableMemoryObjectsFor(
    llvm::Instruction *inst) const {
  std::unordered_set<ClonableMemoryObject *> locations;
  for (const auto &location : this->clonableMemoryLocations) {
    auto *loc = location.get();
    if (loc->getAllocation() == inst ||
        loc->isInstructionCastOrGEPOfLocation(inst) ||
        loc->isInstructionLoadingLocation(inst) ||
        loc->isInstructionStoringLocation(inst)) {
      locations.insert(loc);
      continue;
    }
    if (auto *callInst = llvm::dyn_cast<llvm::CallInst>(inst)) {
      if (callInst->isLifetimeStartOrEnd() && callInst->arg_size() > 1 &&
          loc->mustAliasAMemoryLocationWithinObject(
              callInst->getArgOperand(1))) {
        locations.insert(loc);
      }
    }
  }
  return locations;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
