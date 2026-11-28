/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Memory/LoopIterationSpaceAnalysis.h"

#include "llvm/Analysis/Delinearization.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <queue>

namespace lotus {
namespace analysis {
namespace loop {

namespace {

bool scevsMatch(const llvm::SCEV *scev1, const llvm::SCEV *scev2) {
  if (scev1 == scev2) {
    return true;
  }
  auto *constant1 = llvm::dyn_cast<llvm::SCEVConstant>(scev1);
  auto *constant2 = llvm::dyn_cast<llvm::SCEVConstant>(scev2);
  if (constant1 == nullptr || constant2 == nullptr) {
    return false;
  }
  const auto &value1 = constant1->getValue()->getValue();
  const auto &value2 = constant2->getValue()->getValue();
  return value1.getBitWidth() == value2.getBitWidth() && value1 == value2;
}

const llvm::SCEV *stripSCEVCast(const llvm::SCEV *scev) {
  if (llvm::isa<llvm::SCEVSignExtendExpr>(scev) ||
      llvm::isa<llvm::SCEVTruncateExpr>(scev) ||
      llvm::isa<llvm::SCEVZeroExtendExpr>(scev)) {
    return llvm::cast<llvm::SCEVCastExpr>(scev)->getOperand();
  }
  return scev;
}

} // namespace

LoopIterationSpaceAnalysis::LoopIterationSpaceAnalysis(
    LoopTree *loops,
    InductionVariableManager &ivManager,
    llvm::ScalarEvolution &SE)
    : loops{loops}, ivManager{ivManager} {
  indexIVInstructionSCEVs(SE);
  if (this->ivInstructionsBySCEV.empty()) {
    return;
  }

  computeMemoryAccessSpace(SE);
  identifyIVForMemoryAccessSubscripts(SE);
  identifyNonOverlappingAccessesBetweenIterationsAcrossOneLoopInvocation(SE);
}

bool LoopIterationSpaceAnalysis::
    areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
        llvm::Instruction *from,
        llvm::Instruction *to) const {
  if (from == nullptr || to == nullptr) {
    return false;
  }
  auto itFrom = this->accessSpaceByInstruction.find(from);
  auto itTo = this->accessSpaceByInstruction.find(to);
  if (itFrom == this->accessSpaceByInstruction.end() ||
      itTo == this->accessSpaceByInstruction.end()) {
    return false;
  }
  return this->areMemoryAccessSpaceNotOverlappingOrExactlyTheSame(itFrom->second,
                                                                  itTo->second);
}

bool LoopIterationSpaceAnalysis::
    areMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
        MemoryAccessSpace *space1,
        MemoryAccessSpace *space2) const {
  if (!space1->isAnalyzed || !space2->isAnalyzed) {
    return false;
  }
  if (this->nonOverlappingAccessesBetweenIterations.count(space1) == 0 ||
      this->nonOverlappingAccessesBetweenIterations.count(space2) == 0) {
    return false;
  }
  if (space1 == space2) {
    return true;
  }
  auto it1 = this->spacesThatCannotOverlap.find(space1);
  auto it2 = this->spacesThatCannotOverlap.find(space2);
  if (it1 == this->spacesThatCannotOverlap.end() ||
      it2 == this->spacesThatCannotOverlap.end()) {
    return false;
  }
  return it1->second.count(space2) != 0 || it2->second.count(space1) != 0;
}

bool LoopIterationSpaceAnalysis::
    analyzeToCheckIfMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
        MemoryAccessSpace *space1,
        MemoryAccessSpace *space2) const {
  if (this->nonOverlappingAccessesBetweenIterations.count(space1) == 0 ||
      this->nonOverlappingAccessesBetweenIterations.count(space2) == 0) {
    return false;
  }
  if (space1->memoryAccessorBasePointerSCEV !=
      space2->memoryAccessorBasePointerSCEV) {
    return false;
  }
  if (space1 == space2) {
    return true;
  }
  return this->isMemoryAccessSpaceEquivalentForTopLoopIVSubscript(space1,
                                                                  space2);
}

bool LoopIterationSpaceAnalysis::
    isMemoryAccessSpaceEquivalentForTopLoopIVSubscript(
        MemoryAccessSpace *space1,
        MemoryAccessSpace *space2) const {
  assert(space1 != nullptr);
  assert(space2 != nullptr);

  if (space1->subscriptIVs.empty() ||
      space1->subscriptIVs.size() != space2->subscriptIVs.size() ||
      space1->memoryMinusSCEV != space2->memoryMinusSCEV) {
    return false;
  }

  auto getLoopsForIV =
      [&](InductionVariable *iv) -> std::unordered_set<LoopStructure *> {
    std::unordered_set<LoopStructure *> loopsForIV;
    for (auto *phi : iv->getPHIsInvolvedInComputingIVStep()) {
      loopsForIV.insert(this->loops->getInnermostLoopThatContains(phi));
    }
    return loopsForIV;
  };

  auto *rootLoopStructure = this->loops->getLoop();
  for (unsigned subscriptIdx = 0; subscriptIdx < space1->subscriptIVs.size();
       ++subscriptIdx) {
    auto *iv1 = space1->subscriptIVs[subscriptIdx].second;
    auto *iv2 = space2->subscriptIVs[subscriptIdx].second;
    if (iv1 == nullptr || iv2 == nullptr) {
      return false;
    }

    auto loops1 = getLoopsForIV(iv1);
    auto loops2 = getLoopsForIV(iv2);
    if (loops1 != loops2) {
      for (auto *loop1 : loops1) {
        for (auto *loop2 : loops2) {
          if (rootLoopStructure == loop1 || rootLoopStructure == loop2) {
            return false;
          }
        }
      }
    }

    const auto *scev1 = space1->subscripts[subscriptIdx];
    const auto *scev2 = space2->subscripts[subscriptIdx];
    for (auto *loop : loops1) {
      if (rootLoopStructure == loop && scev1 != scev2) {
        return false;
      }
    }
  }

  return true;
}

void LoopIterationSpaceAnalysis::indexIVInstructionSCEVs(
    llvm::ScalarEvolution &SE) {
  for (auto *loop : this->loops->getLoops()) {
    for (auto *iv : this->ivManager.getInductionVariables(*loop)) {
      for (auto *inst : iv->getAllInstructions()) {
        if (!SE.isSCEVable(inst->getType())) {
          continue;
        }
        const auto *scev = SE.getSCEV(inst);
        this->ivInstructionsBySCEV[scev].insert(inst);
        this->ivsByInstruction.emplace(inst, iv);
      }

      for (auto *inst : iv->getDerivedSCEVInstructions()) {
        if (!SE.isSCEVable(inst->getType())) {
          continue;
        }
        const auto *scev = SE.getSCEV(inst);
        this->derivedInstructionsFromIVsBySCEV[scev].insert(inst);
        this->ivsByInstruction.emplace(inst, iv);
      }
    }
  }
}

void LoopIterationSpaceAnalysis::computeMemoryAccessSpace(
    llvm::ScalarEvolution &SE) {
  std::unordered_set<llvm::Instruction *> memoryAccessors;
  auto *targetLoop = this->loops->getLoop();

  for (auto *B : targetLoop->getBasicBlocks()) {
    for (auto &I : *B) {
      llvm::Value *memoryAccessorValue = nullptr;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        memoryAccessorValue = store->getPointerOperand();
      } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        memoryAccessorValue = load->getPointerOperand();
      } else if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
        memoryAccessorValue = gep;
      } else {
        continue;
      }

      if (auto *memoryAccessor =
              llvm::dyn_cast<llvm::Instruction>(memoryAccessorValue)) {
        memoryAccessors.insert(memoryAccessor);
      }
    }
  }

  for (auto *memoryAccessor : memoryAccessors) {
    if (!SE.isSCEVable(memoryAccessor->getType())) {
      continue;
    }

    this->accessSpaces.push_back(
        std::make_unique<MemoryAccessSpace>(memoryAccessor));
    auto *memAccessSpace = this->accessSpaces.back().get();
    this->accessSpaceByInstruction[memoryAccessor] = memAccessSpace;
    memAccessSpace->memoryAccessorSCEV =
        SE.getSCEV(memAccessSpace->memoryAccessor);

    for (auto *user : memoryAccessor->users()) {
      auto *accessor = llvm::dyn_cast<llvm::Instruction>(user);
      if (accessor == nullptr) {
        continue;
      }
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(accessor)) {
        if (store->getPointerOperand() != memoryAccessor) {
          continue;
        }
        this->accessSpaceByInstruction[store] = memAccessSpace;
        memAccessSpace->accessInstructions.insert(store);
      } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(accessor)) {
        if (load->getPointerOperand() != memoryAccessor) {
          continue;
        }
        this->accessSpaceByInstruction[load] = memAccessSpace;
        memAccessSpace->accessInstructions.insert(load);
      } else if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(accessor)) {
        if (gep->getPointerOperand() == memoryAccessor) {
          this->accessSpaceByInstruction[gep] = memAccessSpace;
        }
      }
    }

    const auto &DL = memoryAccessor->getModule()->getDataLayout();
    for (auto *accessor : memAccessSpace->accessInstructions) {
      llvm::Type *accessedType = nullptr;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(accessor)) {
        accessedType = store->getValueOperand()->getType();
      } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(accessor)) {
        accessedType = load->getType();
      }
      if (accessedType == nullptr || !accessedType->isSized()) {
        continue;
      }
      auto accessSize = DL.getTypeStoreSize(accessedType);
      if (accessSize.isScalable()) {
        memAccessSpace->maxAccessSizeBytes = 0;
        break;
      }
      memAccessSpace->maxAccessSizeBytes = std::max(
          memAccessSpace->maxAccessSizeBytes, accessSize.getFixedSize());
    }
    if (memAccessSpace->maxAccessSizeBytes == 0) {
      continue;
    }

    llvm::Type *accessedType = nullptr;
    if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(memoryAccessor)) {
      accessedType = gep->getResultElementType();
    } else if (!memAccessSpace->accessInstructions.empty()) {
      auto *accessor = *memAccessSpace->accessInstructions.begin();
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(accessor)) {
        accessedType = store->getValueOperand()->getType();
      } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(accessor)) {
        accessedType = load->getType();
      }
    }
    if (accessedType == nullptr) {
      continue;
    }

    auto *ptrToAccessedType = llvm::PointerType::getUnqual(accessedType);
    auto *efType = SE.getEffectiveSCEVType(ptrToAccessedType);
    memAccessSpace->elementSize = SE.getSizeOfExpr(efType, accessedType);
    if (memAccessSpace->elementSize == nullptr) {
      continue;
    }

    memAccessSpace->memoryAccessorBasePointerSCEV =
        llvm::dyn_cast<llvm::SCEVUnknown>(
            SE.getPointerBase(memAccessSpace->memoryAccessorSCEV));
    if (memAccessSpace->memoryAccessorBasePointerSCEV == nullptr) {
      continue;
    }

    const auto *basePointer = memAccessSpace->memoryAccessorBasePointerSCEV;
    const auto *accessFunction =
        SE.getMinusSCEV(memAccessSpace->memoryAccessorSCEV, basePointer);
    memAccessSpace->memoryMinusSCEV = accessFunction;
    llvm::SmallVector<const llvm::SCEV *, 4> delinearizedSubscripts;
    llvm::SmallVector<const llvm::SCEV *, 4> delinearizedSizes;
    llvm::delinearize(SE, accessFunction, delinearizedSubscripts,
                      delinearizedSizes, memAccessSpace->elementSize);
    memAccessSpace->subscripts.assign(delinearizedSubscripts.begin(),
                                      delinearizedSubscripts.end());
    memAccessSpace->sizes.assign(delinearizedSizes.begin(),
                                 delinearizedSizes.end());

    if (memAccessSpace->subscripts.empty()) {
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
              memAccessSpace->memoryAccessor)) {
        llvm::SmallVector<int, 4> sizes;
        llvm::SmallVector<const llvm::SCEV *, 4> gepSubscripts;
        llvm::getIndexExpressionsFromGEP(SE, gep, gepSubscripts, sizes);
        memAccessSpace->subscripts.assign(gepSubscripts.begin(),
                                          gepSubscripts.end());
        for (auto size : sizes) {
          memAccessSpace->sizes.push_back(
              SE.getConstant(accessFunction->getType(), size));
        }
        if (sizes.empty()) {
          memAccessSpace->sizes.push_back(memAccessSpace->elementSize);
        }
      }
    }

    bool isFullyDelinearized = true;
    for (const auto *subscript : memAccessSpace->subscripts) {
      if (auto *addRecSubscript =
              llvm::dyn_cast<llvm::SCEVAddRecExpr>(subscript)) {
        if (llvm::isa<llvm::SCEVAddRecExpr>(addRecSubscript->getStart()) ||
            llvm::isa<llvm::SCEVAddRecExpr>(
                addRecSubscript->getStepRecurrence(SE))) {
          isFullyDelinearized = false;
          break;
        }
      }
    }
    if (isFullyDelinearized) {
      memAccessSpace->isAnalyzed = true;
    } else {
      memAccessSpace->subscripts.clear();
      memAccessSpace->sizes.clear();
    }
  }
}

void LoopIterationSpaceAnalysis::
    identifyNonOverlappingAccessesBetweenIterationsAcrossOneLoopInvocation(
        llvm::ScalarEvolution &SE) {
  auto hasAccessDistanceAtLeastWidth = [&](MemoryAccessSpace *space) {
    if (llvm::isa<llvm::GetElementPtrInst>(space->memoryAccessor)) {
      if (auto *elementSize =
              llvm::dyn_cast<llvm::SCEVConstant>(space->elementSize)) {
        auto size = elementSize->getValue()->getValue();
        unsigned compareWidth = std::max(64u, size.getBitWidth());
        llvm::APInt accessWidth(compareWidth, space->maxAccessSizeBytes);
        if (size.zextOrTrunc(compareWidth).uge(accessWidth)) {
          return true;
        }
      }
    }

    auto *rootHeader = this->loops->getLoop()->getHeader();
    const llvm::SCEVAddRecExpr *rootRecurrence = nullptr;
    std::function<bool(const llvm::SCEV *)> findRootRecurrence =
        [&](const llvm::SCEV *scev) {
          if (auto *addRec = llvm::dyn_cast<llvm::SCEVAddRecExpr>(scev)) {
            if (addRec->getLoop()->getHeader() == rootHeader) {
              if (rootRecurrence != nullptr && rootRecurrence != addRec) {
                return false;
              }
              rootRecurrence = addRec;
              return true;
            }
            return findRootRecurrence(addRec->getStart()) &&
                   findRootRecurrence(addRec->getStepRecurrence(SE));
          }
          if (auto *nary = llvm::dyn_cast<llvm::SCEVNAryExpr>(scev)) {
            for (unsigned i = 0; i < nary->getNumOperands(); ++i) {
              if (!findRootRecurrence(nary->getOperand(i))) {
                return false;
              }
            }
          } else if (auto *cast = llvm::dyn_cast<llvm::SCEVCastExpr>(scev)) {
            return findRootRecurrence(cast->getOperand());
          } else if (auto *udiv = llvm::dyn_cast<llvm::SCEVUDivExpr>(scev)) {
            return findRootRecurrence(udiv->getLHS()) &&
                   findRootRecurrence(udiv->getRHS());
          }
          return true;
        };

    if (!findRootRecurrence(space->memoryAccessorSCEV) ||
        rootRecurrence == nullptr ||
        (!rootRecurrence->hasNoSelfWrap() &&
         !(rootRecurrence->hasNoSignedWrap() &&
           rootRecurrence->hasNoUnsignedWrap()))) {
      return false;
    }

    auto *step = llvm::dyn_cast<llvm::SCEVConstant>(
        rootRecurrence->getStepRecurrence(SE));
    if (step == nullptr || step->getValue()->isZero()) {
      return false;
    }
    auto magnitude = step->getValue()->getValue().abs();
    unsigned compareWidth = std::max(64u, magnitude.getBitWidth());
    auto widenedMagnitude = magnitude.zextOrTrunc(compareWidth);
    llvm::APInt accessWidth(compareWidth, space->maxAccessSizeBytes);
    return widenedMagnitude.uge(accessWidth);
  };

  for (auto &memAccessSpaceOwner : this->accessSpaces) {
    auto *memAccessSpace = memAccessSpaceOwner.get();
    if (memAccessSpace->subscriptIVs.empty() ||
        memAccessSpace->subscriptIVs.size() != memAccessSpace->sizes.size()) {
      continue;
    }

    bool isOverflowPossible = true;
    auto *scevExpression =
        llvm::dyn_cast<llvm::SCEVNAryExpr>(memAccessSpace->memoryAccessorSCEV);
    if (scevExpression != nullptr) {
      bool isBound = true;
      bool foundOffsetExpression = false;
      for (unsigned operandID = 0; operandID < scevExpression->getNumOperands();
           ++operandID) {
        const auto *operand = scevExpression->getOperand(operandID);
        if (llvm::isa<llvm::SCEVConstant>(operand)) {
          continue;
        }
        if (auto *scevUnknown = llvm::dyn_cast<llvm::SCEVUnknown>(operand)) {
          if (llvm::isa<llvm::Argument>(scevUnknown->getValue())) {
            continue;
          }
        }

        auto *offsetExpression = llvm::dyn_cast<llvm::SCEVNAryExpr>(operand);
        if (offsetExpression == nullptr || foundOffsetExpression) {
          isBound = false;
          break;
        }
        foundOffsetExpression = true;
        if (!offsetExpression->hasNoSelfWrap() &&
            !(offsetExpression->hasNoSignedWrap() &&
              offsetExpression->hasNoUnsignedWrap())) {
          isBound = false;
          break;
        }
      }
      if (isBound && foundOffsetExpression) {
        isOverflowPossible = false;
      }
    }

    if (isOverflowPossible) {
      if (!isInnerDimensionSubscriptsBounded(SE, memAccessSpace)) {
        continue;
      }

      bool atLeastOneTopLevelNonOverlappingIV = false;
      auto *rootLoopStructure = this->loops->getLoop();
      for (unsigned idx = 0; idx < memAccessSpace->subscriptIVs.size(); ++idx) {
        auto instIVPair = memAccessSpace->subscriptIVs[idx];
        auto *inst = instIVPair.first;
        auto *iv = instIVPair.second;
        if (iv == nullptr) {
          continue;
        }

        auto *loopEntryPHI = iv->getLoopEntryPHI();
        auto *loopStructure =
            this->loops->getInnermostLoopThatContains(loopEntryPHI);
        if (rootLoopStructure != loopStructure) {
          continue;
        }

        assert((iv->isIVInstruction(inst) ||
                iv->isDerivedFromIVInstructions(inst)) &&
               "Subscript associated to IV has invalid associated instruction");

        if (isOneToOneFunctionOnIV(SE, rootLoopStructure, iv, inst)) {
          atLeastOneTopLevelNonOverlappingIV = true;
        }
      }

      if (!atLeastOneTopLevelNonOverlappingIV) {
        continue;
      }
    }

    if (hasAccessDistanceAtLeastWidth(memAccessSpace)) {
      this->nonOverlappingAccessesBetweenIterations.insert(memAccessSpace);
    }
  }

  for (const auto &accessSpacePair0 : this->accessSpaceByInstruction) {
    auto *accessSpace0 = accessSpacePair0.second;
    for (const auto &accessSpacePair1 : this->accessSpaceByInstruction) {
      auto *accessSpace1 = accessSpacePair1.second;
      if (accessSpace0 == accessSpace1) {
        continue;
      }
      if (this->analyzeToCheckIfMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
              accessSpace0, accessSpace1)) {
        this->spacesThatCannotOverlap[accessSpace0].insert(accessSpace1);
      }
    }
  }
}

void LoopIterationSpaceAnalysis::identifyIVForMemoryAccessSubscripts(
    llvm::ScalarEvolution &SE) {
  auto findCorrespondingIVForSubscript = [&](const llvm::SCEV *subscriptSCEV)
      -> std::pair<llvm::Instruction *, InductionVariable *> {
    auto emptyPair = std::make_pair(nullptr, nullptr);
    if (llvm::isa<llvm::SCEVConstant>(subscriptSCEV)) {
      return emptyPair;
    }

    auto findInstructionInLoopForSCEV =
        [&SE](std::unordered_map<const llvm::SCEV *,
                                 std::unordered_set<llvm::Instruction *>>
                  &scevToInstMap,
              const llvm::SCEV *targetSCEV) -> llvm::Instruction * {
      auto found = scevToInstMap.find(targetSCEV);
      if (found != scevToInstMap.end()) {
        return *found->second.begin();
      }

      if (auto *addRecSubscriptSCEV =
              llvm::dyn_cast<llvm::SCEVAddRecExpr>(targetSCEV)) {
        auto *loopHeader = addRecSubscriptSCEV->getLoop()->getHeader();
        for (const auto &scevInstPair : scevToInstMap) {
          auto *otherAddRecSCEV =
              llvm::dyn_cast<llvm::SCEVAddRecExpr>(scevInstPair.first);
          if (otherAddRecSCEV == nullptr ||
              otherAddRecSCEV->getLoop()->getHeader() != loopHeader) {
            continue;
          }
          if (!scevsMatch(addRecSubscriptSCEV->getStart(),
                          otherAddRecSCEV->getStart()) ||
              !scevsMatch(addRecSubscriptSCEV->getStepRecurrence(SE),
                          otherAddRecSCEV->getStepRecurrence(SE))) {
            continue;
          }
          return *scevInstPair.second.begin();
        }
      }

      return nullptr;
    };

    if (auto *ivInst = findInstructionInLoopForSCEV(this->ivInstructionsBySCEV,
                                                    subscriptSCEV)) {
      return std::make_pair(ivInst, this->ivsByInstruction.at(ivInst));
    }

    if (auto *derivedInst = findInstructionInLoopForSCEV(
            this->derivedInstructionsFromIVsBySCEV, subscriptSCEV)) {
      return std::make_pair(derivedInst,
                            this->ivsByInstruction.at(derivedInst));
    }

    return emptyPair;
  };

  for (auto &memAccessSpaceOwner : this->accessSpaces) {
    auto *memAccessSpace = memAccessSpaceOwner.get();
    int idx = 0;
    for (const auto *subscriptSCEV : memAccessSpace->subscripts) {
      if (llvm::isa<llvm::SCEVConstant>(subscriptSCEV)) {
        memAccessSpace->subscripts[idx] = memAccessSpace->memoryAccessorSCEV;
        subscriptSCEV = memAccessSpace->memoryAccessorSCEV;
      }
      memAccessSpace->subscriptIVs.push_back(
          findCorrespondingIVForSubscript(subscriptSCEV));
      ++idx;
    }
    if (auto *phi =
            llvm::dyn_cast<llvm::PHINode>(memAccessSpace->memoryAccessor)) {
      if (idx == 0 && phi->getNumIncomingValues() == 1) {
        memAccessSpace->subscriptIVs.push_back(findCorrespondingIVForSubscript(
            memAccessSpace->memoryAccessorSCEV));
        assert(memAccessSpace->elementSize != nullptr &&
               "elementSize is nullptr");
        memAccessSpace->sizes.push_back(memAccessSpace->elementSize);
        const auto *expr = SE.getSCEV(phi->getIncomingValue(0));
        assert(expr != nullptr && "Expr is nullptr");
        memAccessSpace->subscripts.push_back(expr);
      }
    }
  }
}

LoopIterationSpaceAnalysis::MemoryAccessSpace::MemoryAccessSpace(
    llvm::Instruction *memoryAccessor)
    : memoryAccessor{memoryAccessor} {}

bool LoopIterationSpaceAnalysis::isOneToOneFunctionOnIV(
    llvm::ScalarEvolution &SE,
    LoopStructure *loopStructure,
    InductionVariable *IV,
    llvm::Instruction *derivedInstruction) const {
  auto isInjectiveCast = [](llvm::Instruction *inst) -> bool {
    auto *castInst = llvm::dyn_cast<llvm::CastInst>(inst);
    if (castInst == nullptr) {
      return false;
    }

    auto *srcTy = castInst->getSrcTy();
    auto *dstTy = castInst->getDestTy();
    if (!srcTy->isIntegerTy() || !dstTy->isIntegerTy()) {
      return false;
    }

    auto srcWidth = llvm::cast<llvm::IntegerType>(srcTy)->getBitWidth();
    auto dstWidth = llvm::cast<llvm::IntegerType>(dstTy)->getBitWidth();
    if (llvm::isa<llvm::SExtInst>(castInst) ||
        llvm::isa<llvm::ZExtInst>(castInst)) {
      return dstWidth >= srcWidth;
    }
    if (llvm::isa<llvm::BitCastInst>(castInst)) {
      return dstWidth == srcWidth;
    }

    return false;
  };

  auto hasNonWrappingAddRec = [&](llvm::Instruction *inst) -> bool {
    if (inst == nullptr || !SE.isSCEVable(inst->getType())) {
      return false;
    }
    auto *addRec = llvm::dyn_cast<llvm::SCEVAddRecExpr>(SE.getSCEV(inst));
    return addRec != nullptr &&
           (addRec->hasNoSelfWrap() ||
            (addRec->hasNoSignedWrap() && addRec->hasNoUnsignedWrap()));
  };

  auto isInjectiveMul = [&](llvm::Instruction *inst) -> bool {
    if (inst == nullptr || inst->getOpcode() != llvm::Instruction::Mul ||
        !hasNonWrappingAddRec(inst)) {
      return false;
    }

    unsigned ivDerivedOperands = 0;
    for (auto &use : inst->operands()) {
      auto *operand = use.get();
      if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(operand)) {
        if (constant->isZero()) {
          return false;
        }
        continue;
      }
      auto *operandInst = llvm::dyn_cast<llvm::Instruction>(operand);
      if (this->ivManager.doesContributeToComputeAnInductionVariable(
              operandInst)) {
        if (!loopStructure->isIncluded(operandInst) ||
            (!IV->isIVInstruction(operandInst) &&
             !IV->isDerivedFromIVInstructions(operandInst))) {
          return false;
        }
        ++ivDerivedOperands;
        continue;
      }
      if (operandInst != nullptr && loopStructure->isIncluded(operandInst)) {
        return false;
      }
      if (!SE.isSCEVable(operand->getType()) ||
          !SE.isKnownNonZero(SE.getSCEV(operand))) {
        return false;
      }
    }

    return ivDerivedOperands == 1;
  };

  auto isInjectiveAddOrSub = [&](llvm::Instruction *inst) -> bool {
    if (inst == nullptr ||
        (inst->getOpcode() != llvm::Instruction::Add &&
         inst->getOpcode() != llvm::Instruction::Sub) ||
        !hasNonWrappingAddRec(inst)) {
      return false;
    }

    unsigned ivDerivedOperands = 0;
    for (auto &use : inst->operands()) {
      auto *operandInst = llvm::dyn_cast<llvm::Instruction>(use.get());
      if (operandInst == nullptr || !loopStructure->isIncluded(operandInst)) {
        continue;
      }
      if (!IV->isIVInstruction(operandInst) &&
          !IV->isDerivedFromIVInstructions(operandInst)) {
        return false;
      }
      ++ivDerivedOperands;
    }
    return ivDerivedOperands == 1;
  };

  std::queue<llvm::Instruction *> derivingInsts;
  std::unordered_set<llvm::Instruction *> visited;
  derivingInsts.push(derivedInstruction);

  while (!derivingInsts.empty()) {
    auto *inst = derivingInsts.front();
    derivingInsts.pop();
    if (IV->isIVInstruction(inst)) {
      if (!hasNonWrappingAddRec(inst)) {
        return false;
      }
      continue;
    }

    if (!isInjectiveAddOrSub(inst) && !isInjectiveMul(inst) &&
        !isInjectiveCast(inst)) {
      return false;
    }

    for (auto &use : inst->operands()) {
      auto *usedValue = use.get();
      if (llvm::isa<llvm::ConstantInt>(usedValue)) {
        continue;
      }
      auto *usedInst = llvm::dyn_cast<llvm::Instruction>(usedValue);
      if (usedInst == nullptr) {
        return false;
      }
      if (!loopStructure->isIncluded(usedInst) ||
          !visited.insert(usedInst).second) {
        continue;
      }
      derivingInsts.push(usedInst);
    }
  }

  return true;
}

bool LoopIterationSpaceAnalysis::isInnerDimensionSubscriptsBounded(
    llvm::ScalarEvolution &SE,
    MemoryAccessSpace *space) {
  if (space->subscriptIVs.empty() ||
      space->subscriptIVs.size() != space->sizes.size()) {
    return false;
  }

  for (unsigned i = 1; i < space->sizes.size(); ++i) {
    const auto *sizeSCEV = space->sizes[i - 1];
    auto instIVPair = space->subscriptIVs[i];
    auto *inst = instIVPair.first;
    auto *iv = instIVPair.second;
    if (inst == nullptr) {
      return false;
    }

    const auto *subscriptSCEV = SE.getSCEV(inst);
    if (subscriptSCEV == nullptr) {
      return false;
    }
    subscriptSCEV = stripSCEVCast(subscriptSCEV);
    if (!llvm::isa<llvm::IntegerType>(subscriptSCEV->getType())) {
      return false;
    }

    if (iv != nullptr && iv->isIVInstruction(inst) &&
        llvm::isa<llvm::SCEVAddRecExpr>(subscriptSCEV)) {
      auto *subscriptRecSCEV =
          llvm::cast<llvm::SCEVAddRecExpr>(subscriptSCEV);
      auto *loopEntryPHI = iv->getLoopEntryPHI();
      auto *loopEntryPHISCEV =
          llvm::dyn_cast<llvm::SCEVAddRecExpr>(SE.getSCEV(loopEntryPHI));
      if (loopEntryPHISCEV == nullptr) {
        return false;
      }

      const auto *stepSCEV = subscriptRecSCEV->getStepRecurrence(SE);
      auto *constantStepSCEV = llvm::dyn_cast<llvm::SCEVConstant>(stepSCEV);
      if (constantStepSCEV != nullptr &&
          constantStepSCEV->getValue()->isNegative()) {
        return false;
      }

      if (scevsMatch(subscriptRecSCEV->getStart(),
                     loopEntryPHISCEV->getStart()) &&
          scevsMatch(stepSCEV, loopEntryPHISCEV->getStepRecurrence(SE))) {
        auto *loopHeader = loopEntryPHI->getParent();
        auto *loopStructure =
            this->loops->getInnermostLoopThatContains(loopHeader);
        if (loopStructure == nullptr) {
          return false;
        }
        auto *attr =
            ivManager.getLoopGoverningInductionVariable(*loopStructure);
        if (attr != nullptr && iv == attr->getInductionVariable() &&
            constantStepSCEV != nullptr &&
            !constantStepSCEV->getValue()->isNegative()) {
          auto *conditionValue = attr->getExitConditionValue();
          auto *cmpInst =
              attr->getHeaderCompareInstructionToComputeExitCondition();
          if (conditionValue == nullptr || cmpInst == nullptr) {
            return false;
          }

          auto predicate = cmpInst->getPredicate();
          auto *exitBlock = attr->getExitBlockFromHeader();
          auto *falseSuccessor = *(++llvm::succ_begin(loopHeader));
          bool exitOnFalse = exitBlock == falseSuccessor;
          bool isConditionLHS = cmpInst->getOperand(0) == conditionValue;

          if (predicate == llvm::ICmpInst::Predicate::ICMP_ULE ||
              predicate == llvm::ICmpInst::Predicate::ICMP_SLE) {
            predicate = llvm::ICmpInst::Predicate::ICMP_UGT;
            isConditionLHS = !isConditionLHS;
          } else if (predicate == llvm::ICmpInst::Predicate::ICMP_UGE ||
                     predicate == llvm::ICmpInst::Predicate::ICMP_SGE) {
            predicate = llvm::ICmpInst::Predicate::ICMP_ULT;
            isConditionLHS = !isConditionLHS;
          }

          bool isUpperBoundedByEq =
              (!exitOnFalse &&
               predicate == llvm::ICmpInst::Predicate::ICMP_EQ) ||
              (exitOnFalse && predicate == llvm::ICmpInst::Predicate::ICMP_NE);
          bool isUpperBoundedByLT =
              exitOnFalse && !isConditionLHS &&
              (predicate == llvm::ICmpInst::Predicate::ICMP_ULT ||
               predicate == llvm::ICmpInst::Predicate::ICMP_SLT);
          bool isUpperBoundedByFlippedGT =
              exitOnFalse && isConditionLHS &&
              (predicate == llvm::ICmpInst::Predicate::ICMP_UGT ||
               predicate == llvm::ICmpInst::Predicate::ICMP_SGT);

          if (isUpperBoundedByEq || isUpperBoundedByLT ||
              isUpperBoundedByFlippedGT) {
            const auto *conditionSCEVBase =
                stripSCEVCast(SE.getSCEV(conditionValue));
            const auto *sizeSCEVBase = stripSCEVCast(sizeSCEV);

            if (conditionSCEVBase == sizeSCEVBase) {
              continue;
            }

            if (auto *conditionOffsetSCEV =
                    llvm::dyn_cast<llvm::SCEVAddExpr>(conditionSCEVBase)) {
              if (conditionOffsetSCEV->getNumOperands() == 2) {
                const auto *lhsSCEV = conditionOffsetSCEV->getOperand(0);
                const auto *rhsSCEV = conditionOffsetSCEV->getOperand(1);
                if ((lhsSCEV == sizeSCEVBase) ^ (rhsSCEV == sizeSCEVBase)) {
                  const auto *otherSCEV =
                      lhsSCEV == sizeSCEVBase ? rhsSCEV : lhsSCEV;
                  if (auto *constOffsetSCEV =
                          llvm::dyn_cast<llvm::SCEVConstant>(otherSCEV)) {
                    if (constOffsetSCEV->getValue()->isNegative()) {
                      continue;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    return false;
  }

  return true;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
