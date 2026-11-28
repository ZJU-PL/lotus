/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Scalar/LoopGoverningInductionVariable.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

#include <cassert>
#include <queue>

namespace lotus {
namespace analysis {
namespace loop {

LoopGoverningInductionVariable::LoopGoverningInductionVariable(
    LoopStructure *loop,
    InductionVariable &iv)
    : LoopGoverningInductionVariable(loop, iv, loop->getLoopExitBasicBlocks()) {
}

LoopGoverningInductionVariable::LoopGoverningInductionVariable(
    LoopStructure *loop,
    InductionVariable &iv,
    LoopSCC &,
    const std::vector<llvm::BasicBlock *> &exitBlocks)
    : loop{loop},
      iv{&iv},
      headerCmp{nullptr},
      headerBr{nullptr},
      conditionValue{nullptr},
      comparedValue{nullptr},
      exitBlock{nullptr},
      isWellFormed{false} {
  assert(loop != nullptr);

  if (iv.getSingleComputedStepValue() == nullptr ||
      (!llvm::isa<llvm::ConstantInt>(iv.getSingleComputedStepValue()) &&
       !llvm::isa<llvm::ConstantFP>(iv.getSingleComputedStepValue()))) {
    return;
  }

  auto *headerPHI = iv.getLoopEntryPHI();
  auto ivInstructions = iv.getAllInstructions();
  if (!headerPHI->getType()->isIntegerTy() &&
      !headerPHI->getType()->isFloatingPointTy()) {
    return;
  }

  auto *sccOfIV = iv.getSCC();
  if (sccOfIV == nullptr) {
    return;
  }

  llvm::BranchInst *loopGoverningTerminator = nullptr;
  for (const auto &pair : sccOfIV->internalNodePairs()) {
    auto *value = pair.first;
    if (llvm::isa<llvm::InvokeInst>(value)) {
      return;
    }
    auto *br = llvm::dyn_cast<llvm::BranchInst>(value);
    if (br == nullptr || !br->isConditional()) {
      continue;
    }
    if (loopGoverningTerminator != nullptr) {
      return;
    }
    loopGoverningTerminator = br;
  }

  if (loopGoverningTerminator == nullptr ||
      loopGoverningTerminator->getParent() != headerPHI->getParent()) {
    return;
  }

  this->headerBr = loopGoverningTerminator;
  auto *headerCondition = this->headerBr->getCondition();
  auto *cmp = llvm::dyn_cast<llvm::CmpInst>(headerCondition);
  if (cmp == nullptr) {
    return;
  }

  this->headerCmp = cmp;
  auto *opL = cmp->getOperand(0);
  auto *opR = cmp->getOperand(1);
  auto isOpLHSLoopEntryPHI =
      llvm::isa<llvm::Instruction>(opL) &&
      headerPHI == llvm::cast<llvm::Instruction>(opL);
  auto isOpRHSLoopEntryPHI =
      llvm::isa<llvm::Instruction>(opR) &&
      headerPHI == llvm::cast<llvm::Instruction>(opR);
  if (!(isOpLHSLoopEntryPHI ^ isOpRHSLoopEntryPHI)) {
    for (auto *intermediateValue : iv.getNonPHIIntermediateValues()) {
      if (intermediateValue == opR || intermediateValue == opL) {
        this->comparedValue = intermediateValue;
        break;
      }
    }
    if (this->comparedValue == nullptr) {
      return;
    }
    this->conditionValue = (this->comparedValue == opR) ? opL : opR;
  } else {
    this->conditionValue = isOpLHSLoopEntryPHI ? opR : opL;
    this->comparedValue =
        llvm::cast<llvm::Instruction>(isOpLHSLoopEntryPHI ? opL : opR);
  }

  std::set<llvm::BasicBlock *> exitBlockSet(exitBlocks.begin(),
                                            exitBlocks.end());
  if (exitBlockSet.count(this->headerBr->getSuccessor(0)) != 0) {
    this->exitBlock = this->headerBr->getSuccessor(0);
  } else if (exitBlockSet.count(this->headerBr->getSuccessor(1)) != 0) {
    this->exitBlock = this->headerBr->getSuccessor(1);
  } else {
    return;
  }

  if (sccOfIV->isInternal(this->conditionValue)) {
    auto *conditionInst = llvm::dyn_cast<llvm::Instruction>(this->conditionValue);
    if (conditionInst == nullptr) {
      return;
    }
    std::queue<llvm::Instruction *> conditionDerivation;
    conditionDerivation.push(conditionInst);
    while (!conditionDerivation.empty()) {
      auto *value = conditionDerivation.front();
      conditionDerivation.pop();

      auto *valueNode = sccOfIV->fetchNode(value);
      if (valueNode == nullptr) {
        continue;
      }
      for (auto *edge : valueNode->getIncomingEdges()) {
        if (edge->getKind() != LoopDependenceEdgeKind::Variable) {
          continue;
        }
        auto *outgoingInst = llvm::dyn_cast_or_null<llvm::Instruction>(
            edge->getSrc()->getValue());
        if (outgoingInst == nullptr || !sccOfIV->isInternal(outgoingInst)) {
          continue;
        }
        if (ivInstructions.count(outgoingInst) != 0) {
          return;
        }
        if (!this->conditionValueDerivation.insert(outgoingInst).second) {
          continue;
        }
        conditionDerivation.push(outgoingInst);
      }
    }
  }

  this->isWellFormed = true;
}

LoopGoverningInductionVariable::LoopGoverningInductionVariable(
    LoopStructure *loop,
    InductionVariable &iv,
    const std::vector<llvm::BasicBlock *> &exitBlocks)
    : loop{loop},
      iv{&iv},
      headerCmp{nullptr},
      headerBr{nullptr},
      conditionValue{nullptr},
      comparedValue{nullptr},
      exitBlock{nullptr},
      isWellFormed{false} {
  auto *scc = iv.getSCC();
  if (scc == nullptr) {
    return;
  }

  LoopGoverningInductionVariable scoped(loop, iv, *scc, exitBlocks);
  this->conditionValueDerivation = scoped.getConditionValueDerivation();
  this->headerCmp = scoped.getHeaderCompareInstructionToComputeExitCondition();
  this->headerBr = scoped.getHeaderBrInst();
  this->conditionValue = scoped.getExitConditionValue();
  this->comparedValue = scoped.getValueToCompareAgainstExitConditionValue();
  this->exitBlock = scoped.getExitBlockFromHeader();
  this->isWellFormed = scoped.isSCCContainingIVWellFormed();
}

InductionVariable *
LoopGoverningInductionVariable::getInductionVariable(void) const {
  return this->iv;
}

llvm::CmpInst *LoopGoverningInductionVariable::
    getHeaderCompareInstructionToComputeExitCondition(void) const {
  return this->headerCmp;
}

llvm::Value *
LoopGoverningInductionVariable::getExitConditionValue(void) const {
  return this->conditionValue;
}

llvm::BranchInst *LoopGoverningInductionVariable::getHeaderBrInst(void) const {
  return this->headerBr;
}

llvm::BasicBlock *
LoopGoverningInductionVariable::getExitBlockFromHeader(void) const {
  return this->exitBlock;
}

bool LoopGoverningInductionVariable::valueOfExitConditionToJumpToTheLoopBody(
    void) const {
  assert(this->headerBr != nullptr);
  auto *succTrue = this->headerBr->getSuccessor(0);
  return this->loop->isIncluded(succTrue);
}

bool LoopGoverningInductionVariable::isSCCContainingIVWellFormed(void) const {
  return this->isWellFormed;
}

std::set<llvm::Instruction *>
LoopGoverningInductionVariable::getConditionValueDerivation(void) const {
  return this->conditionValueDerivation;
}

llvm::Instruction *
LoopGoverningInductionVariable::getValueToCompareAgainstExitConditionValue(
    void) const {
  return this->comparedValue;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
