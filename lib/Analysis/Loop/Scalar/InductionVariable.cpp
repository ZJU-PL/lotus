/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Scalar/InductionVariable.h"
#include "Analysis/Loop/Scalar/ScalarEvolutionReferencer.h"

#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

#include <cassert>
#include <functional>
#include <queue>

namespace lotus {
namespace analysis {
namespace loop {

InductionVariable::InductionVariable(
    LoopStructure *loop,
    LoopSCC *scc,
    llvm::PHINode *loopEntryPHI,
    llvm::Value *startValue,
    const llvm::SCEV *stepSCEV,
    llvm::Value *singleStepValue,
    std::unordered_set<llvm::PHINode *> stepPHIs,
    std::unordered_set<llvm::PHINode *> phis,
    std::unordered_set<llvm::Instruction *> nonPHIInstructions,
    std::unordered_set<llvm::Instruction *> instructions,
    std::unordered_set<llvm::Instruction *> derivedInstructions)
    : loop{loop},
      scc{scc},
      loopEntryPHI{loopEntryPHI},
      stepPHIs{std::move(stepPHIs)},
      phis{std::move(phis)},
      nonPHIInstructions{std::move(nonPHIInstructions)},
      startValue{startValue},
      stepSCEV{stepSCEV},
      singleStepValue{singleStepValue},
      stepMultiplier{1},
      isComputedStepValueLoopInvariant{singleStepValue != nullptr &&
                                       loop != nullptr &&
                                       loop->isLoopInvariant(singleStepValue)},
      loopEntryPHIType{loopEntryPHI ? loopEntryPHI->getType() : nullptr},
      instructions{std::move(instructions)},
      derivedInstructions{std::move(derivedInstructions)} {}

InductionVariable::InductionVariable(
    LoopStructure *loop,
    InvariantManager &IVM,
    llvm::ScalarEvolution &SE,
    llvm::PHINode *loopEntryPHI,
    LoopSCC *scc,
    LoopEnvironment &loopEnvironment,
    detail::ScalarEvolutionReferentialExpander &referentialExpander,
    llvm::InductionDescriptor &descriptor)
    : loop{loop},
      scc{scc},
      loopEntryPHI{loopEntryPHI},
      stepPHIs{loopEntryPHI},
      startValue{descriptor.getStartValue()},
      stepSCEV{descriptor.getStep()},
      singleStepValue{descriptor.getConstIntStepValue()},
      stepMultiplier{1},
      isComputedStepValueLoopInvariant{false},
      loopEntryPHIType{loopEntryPHI->getType()} {
  this->traverseCycleThroughLoopEntryPHIToGetAllIVInstructions();
  this->traverseConsumersOfIVInstructionsToGetAllDerivedSCEVInstructions(IVM,
                                                                        SE);
  this->collectValuesInternalAndExternalToLoopAndSCC(loopEnvironment);

  if (descriptor.getKind() ==
      llvm::InductionDescriptor::InductionKind::IK_FpInduction) {
    this->singleStepValue =
        llvm::cast<llvm::SCEVUnknown>(this->stepSCEV)->getValue();
    this->isComputedStepValueLoopInvariant = true;
  } else {
    this->deriveStepValue(SE, referentialExpander, this->stepMultiplier);
  }
}

InductionVariable::InductionVariable(
    LoopStructure *loop,
    InvariantManager &IVM,
    llvm::ScalarEvolution &SE,
    int64_t stepMultiplier,
    llvm::PHINode *loopEntryPHI,
    std::unordered_set<llvm::PHINode *> stepPHIs,
    LoopSCC *scc,
    LoopEnvironment &loopEnvironment,
    detail::ScalarEvolutionReferentialExpander &referentialExpander)
    : loop{loop},
      scc{scc},
      loopEntryPHI{loopEntryPHI},
      stepPHIs{std::move(stepPHIs)},
      startValue{nullptr},
      stepSCEV{nullptr},
      singleStepValue{nullptr},
      stepMultiplier{stepMultiplier},
      isComputedStepValueLoopInvariant{false},
      loopEntryPHIType{loopEntryPHI->getType()} {
  const auto &bbs = loop->getBasicBlocks();
  for (unsigned i = 0; i < loopEntryPHI->getNumIncomingValues(); ++i) {
    auto *incomingBB = loopEntryPHI->getIncomingBlock(i);
    if (bbs.find(incomingBB) == bbs.end()) {
      this->startValue = loopEntryPHI->getIncomingValue(i);
      break;
    }
  }

  this->traverseCycleThroughLoopEntryPHIToGetAllIVInstructions();
  this->traverseConsumersOfIVInstructionsToGetAllDerivedSCEVInstructions(IVM,
                                                                        SE);
  this->collectValuesInternalAndExternalToLoopAndSCC(loopEnvironment);
  this->deriveStepValue(SE, referentialExpander, this->stepMultiplier);
}

InductionVariable::~InductionVariable() = default;

InductionVariable::CollectedInstructions InductionVariable::collectIVInstructions(
    LoopStructure &loop,
    LoopSCC *scc,
    llvm::PHINode *loopEntryPHI,
    bool includeTransitiveCasts) {
  CollectedInstructions result;
  if (scc == nullptr || loopEntryPHI == nullptr) {
    return result;
  }

  std::queue<LoopDependenceNode *> ivIntermediateValues;
  std::set<llvm::Value *> valuesVisited;
  auto *rootNode = scc->fetchNode(loopEntryPHI);
  if (rootNode != nullptr) {
    ivIntermediateValues.push(rootNode);
  }

  while (!ivIntermediateValues.empty()) {
    auto *node = ivIntermediateValues.front();
    ivIntermediateValues.pop();

    auto *value = node->getValue();
    if (!valuesVisited.insert(value).second) {
      continue;
    }

    auto *instruction = llvm::dyn_cast_or_null<llvm::Instruction>(value);
    if (instruction == nullptr || !loop.isIncluded(instruction)) {
      continue;
    }
    result.instructions.insert(instruction);
    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(instruction)) {
      result.phis.insert(phi);
    } else {
      result.nonPHIInstructions.insert(instruction);
    }

    for (auto *edge : node->getIncomingEdges()) {
      if (edge->getKind() != LoopDependenceEdgeKind::Variable) {
        continue;
      }
      auto *otherNode = edge->getSrc();
      if (otherNode == nullptr || !scc->isInternal(otherNode->getValue())) {
        continue;
      }
      ivIntermediateValues.push(otherNode);
    }
  }

  if (result.instructions.empty() && includeTransitiveCasts) {
    result.instructions.insert(loopEntryPHI);
    result.phis.insert(loopEntryPHI);
    for (unsigned i = 0; i < loopEntryPHI->getNumIncomingValues(); ++i) {
      if (!loop.isIncluded(loopEntryPHI->getIncomingBlock(i))) {
        continue;
      }
      if (auto *inst =
              llvm::dyn_cast<llvm::Instruction>(loopEntryPHI->getIncomingValue(i))) {
        result.instructions.insert(inst);
        if (auto *innerPhi = llvm::dyn_cast<llvm::PHINode>(inst)) {
          result.phis.insert(innerPhi);
        } else {
          result.nonPHIInstructions.insert(inst);
        }
      }
    }
  }

  if (includeTransitiveCasts) {
    std::vector<llvm::Instruction *> worklist(result.instructions.begin(),
                                              result.instructions.end());
    while (!worklist.empty()) {
      auto *current = worklist.back();
      worklist.pop_back();
      for (auto *user : current->users()) {
        auto *userInst = llvm::dyn_cast<llvm::Instruction>(user);
        if (userInst == nullptr || !loop.isIncluded(userInst) ||
            !llvm::isa<llvm::CastInst>(userInst)) {
          continue;
        }
        if (result.instructions.insert(userInst).second) {
          result.nonPHIInstructions.insert(userInst);
          worklist.push_back(userInst);
        }
      }
    }
  } else {
    std::set<llvm::CastInst *> castsToAdd;
    for (auto *intermediateValue : result.instructions) {
      for (auto *user : intermediateValue->users()) {
        auto *castInst = llvm::dyn_cast<llvm::CastInst>(user);
        if (castInst != nullptr && loop.isIncluded(castInst)) {
          castsToAdd.insert(castInst);
        }
      }
    }
    result.instructions.insert(castsToAdd.begin(), castsToAdd.end());
    result.nonPHIInstructions.insert(castsToAdd.begin(), castsToAdd.end());
  }

  return result;
}

std::unordered_set<llvm::Instruction *>
InductionVariable::collectDerivedSCEVInstructions(
    LoopStructure &loop,
    InvariantManager &IVM,
    llvm::ScalarEvolution &SE,
    const std::unordered_set<llvm::Instruction *> &ivInstructions,
    bool allowBinaryOps) {
  std::unordered_set<llvm::Instruction *> derivedInstructions;
  std::unordered_set<llvm::Instruction *> checked;

  std::function<bool(llvm::Instruction *)> checkIfDerived;
  checkIfDerived = [&](llvm::Instruction *inst) -> bool {
    if (derivedInstructions.count(inst) != 0) {
      return true;
    }
    if (!checked.insert(inst).second) {
      return false;
    }
    if (!SE.isSCEVable(inst->getType()) || !loop.isIncluded(inst)) {
      return false;
    }

    auto *scev = SE.getSCEV(inst);
    bool supportedSCEV = llvm::isa<llvm::SCEVCastExpr>(scev) ||
                         llvm::isa<llvm::SCEVNAryExpr>(scev) ||
                         llvm::isa<llvm::SCEVUDivExpr>(scev);
    if (!supportedSCEV && !(allowBinaryOps && inst->isBinaryOp())) {
      return false;
    }

    bool usesAtLeastOneIVInstruction = false;
    for (auto &use : inst->operands()) {
      auto *usedValue = use.get();
      if (llvm::isa<llvm::ConstantInt>(usedValue) ||
          IVM.isLoopInvariant(usedValue)) {
        continue;
      }

      auto *usedInst = llvm::dyn_cast<llvm::Instruction>(usedValue);
      if (usedInst == nullptr) {
        return false;
      }
      if (!loop.isIncluded(usedInst)) {
        continue;
      }

      if (ivInstructions.count(usedInst) != 0 || checkIfDerived(usedInst)) {
        usesAtLeastOneIVInstruction = true;
        continue;
      }
      return false;
    }

    if (!usesAtLeastOneIVInstruction) {
      return false;
    }
    derivedInstructions.insert(inst);
    return true;
  };

  std::queue<llvm::Instruction *> worklist;
  std::unordered_set<llvm::Instruction *> visited;
  for (auto *ivInst : ivInstructions) {
    worklist.push(ivInst);
    visited.insert(ivInst);
  }

  while (!worklist.empty()) {
    auto *inst = worklist.front();
    worklist.pop();

    for (auto *user : inst->users()) {
      auto *userInst = llvm::dyn_cast<llvm::Instruction>(user);
      if (userInst == nullptr || !visited.insert(userInst).second) {
        continue;
      }
      if (checkIfDerived(userInst)) {
        worklist.push(userInst);
      }
    }
  }

  return derivedInstructions;
}

void InductionVariable::traverseCycleThroughLoopEntryPHIToGetAllIVInstructions(
    void) {
  auto collected = collectIVInstructions(*this->loop, this->scc,
                                         this->loopEntryPHI, false);
  this->phis = std::move(collected.phis);
  this->nonPHIInstructions = std::move(collected.nonPHIInstructions);
  this->instructions = std::move(collected.instructions);
}

void InductionVariable::
    traverseConsumersOfIVInstructionsToGetAllDerivedSCEVInstructions(
        InvariantManager &IVM,
        llvm::ScalarEvolution &SE) {
  this->derivedInstructions = collectDerivedSCEVInstructions(
      *this->loop, IVM, SE, this->instructions, false);
}

void InductionVariable::collectValuesInternalAndExternalToLoopAndSCC(
    LoopEnvironment &loopEnvironment) {
  if (this->scc != nullptr) {
    for (const auto &internalNodePair : this->scc->internalNodePairs()) {
      this->valuesInScopeOfInductionVariable.insert(internalNodePair.first);
    }
    for (const auto &externalPair : this->scc->externalNodePairs()) {
      this->valuesInScopeOfInductionVariable.insert(externalPair.first);
    }
  }

  for (auto *liveIn : loopEnvironment.getProducers()) {
    this->valuesInScopeOfInductionVariable.insert(liveIn);
    this->valuesToReferenceInComputingStepValue.insert(liveIn);
  }
}

void InductionVariable::deriveStepValue(
    llvm::ScalarEvolution &SE,
    detail::ScalarEvolutionReferentialExpander &referentialExpander,
    int64_t multiplier) {
  if (this->stepSCEV == nullptr) {
    assert(this->stepPHIs.size() == 1 &&
           "Expected one PHI for step value calculation");
    auto *stepSCEVPHI = *this->stepPHIs.begin();
    assert(SE.getSCEV(stepSCEVPHI)->getSCEVType() ==
           llvm::SCEVTypes::scAddRecExpr);
    this->stepSCEV = llvm::cast<llvm::SCEVAddRecExpr>(SE.getSCEV(stepSCEVPHI))
                         ->getStepRecurrence(SE);
  }
  assert(this->stepSCEV != nullptr && "stepSCEV is nullptr");

  switch (this->stepSCEV->getSCEVType()) {
  case llvm::SCEVTypes::scConstant:
    deriveStepValueFromSCEVConstant(
        llvm::cast<llvm::SCEVConstant>(this->stepSCEV), multiplier);
    break;
  case llvm::SCEVTypes::scUnknown:
    deriveStepValueFromSCEVUnknown(
        llvm::cast<llvm::SCEVUnknown>(this->stepSCEV));
    break;
  case llvm::SCEVTypes::scAddExpr:
  case llvm::SCEVTypes::scAddRecExpr:
  case llvm::SCEVTypes::scMulExpr:
  case llvm::SCEVTypes::scSignExtend:
  case llvm::SCEVTypes::scSMaxExpr:
  case llvm::SCEVTypes::scSMinExpr:
  case llvm::SCEVTypes::scTruncate:
  case llvm::SCEVTypes::scUDivExpr:
  case llvm::SCEVTypes::scUMaxExpr:
  case llvm::SCEVTypes::scUMinExpr:
  case llvm::SCEVTypes::scZeroExtend:
    if (!deriveStepValueFromCompositeSCEV(this->stepSCEV,
                                          referentialExpander)) {
      this->stepSCEV = nullptr;
    }
    break;
  default:
    this->stepSCEV = nullptr;
    break;
  }
}

void InductionVariable::deriveStepValueFromSCEVConstant(
    const llvm::SCEVConstant *scev,
    int64_t multiplier) {
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(scev->getValue())) {
    this->singleStepValue = llvm::ConstantInt::get(
        scev->getValue()->getType(), multiplier * CI->getSExtValue());
  } else {
    this->singleStepValue = scev->getValue();
  }
  this->isComputedStepValueLoopInvariant = true;
}

void InductionVariable::deriveStepValueFromSCEVUnknown(
    const llvm::SCEVUnknown *scev) {
  this->singleStepValue = scev->getValue();
  this->isComputedStepValueLoopInvariant =
      this->loop->isLoopInvariant(this->singleStepValue);
}

bool InductionVariable::deriveStepValueFromCompositeSCEV(
    const llvm::SCEV *scev,
    detail::ScalarEvolutionReferentialExpander &referentialExpander) {
  auto stepSizeReferenceTree = referentialExpander.createReferenceTree(
      scev, this->valuesInScopeOfInductionVariable);
  if (stepSizeReferenceTree == nullptr) {
    return false;
  }

  auto tempBlock = std::unique_ptr<llvm::BasicBlock>(
      llvm::BasicBlock::Create(this->loopEntryPHI->getContext(),
                               "temp_basic_block"));
  llvm::IRBuilder<> tempBuilder(tempBlock.get());
  auto *finalValue = referentialExpander.expandUsingReferenceValues(
      stepSizeReferenceTree.get(), this->valuesToReferenceInComputingStepValue,
      tempBuilder);
  if (finalValue == nullptr) {
    return false;
  }

  this->isComputedStepValueLoopInvariant = true;
  for (auto *reference : stepSizeReferenceTree->collectAllReferences()) {
    auto *referenceValue = reference->getValue();
    if (referenceValue == nullptr ||
        !this->loop->isLoopInvariant(referenceValue)) {
      this->isComputedStepValueLoopInvariant = false;
      break;
    }
  }

  if (tempBlock->size() < 2) {
    this->singleStepValue = finalValue;
  }

  for (auto &inst : *tempBlock) {
    this->computationOfStepValue.push_back(&inst);
  }

  this->stepComputationScratchBlock = std::move(tempBlock);
  return true;
}

LoopSCC *InductionVariable::getSCC(void) const { return this->scc; }

llvm::PHINode *InductionVariable::getLoopEntryPHI(void) const {
  return this->loopEntryPHI;
}

std::unordered_set<llvm::PHINode *>
InductionVariable::getPHIsInvolvedInComputingIVStep(void) const {
  return this->stepPHIs;
}

std::unordered_set<llvm::PHINode *> InductionVariable::getPHIs(void) const {
  return this->phis;
}

std::unordered_set<llvm::Instruction *>
InductionVariable::getNonPHIIntermediateValues(void) const {
  return this->nonPHIInstructions;
}

std::unordered_set<llvm::Instruction *>
InductionVariable::getAllInstructions(void) const {
  return this->instructions;
}

std::unordered_set<llvm::Instruction *>
InductionVariable::getDerivedSCEVInstructions(void) const {
  return this->derivedInstructions;
}

llvm::Value *InductionVariable::getStartValue(void) const {
  return this->startValue;
}

llvm::Value *InductionVariable::getSingleComputedStepValue(void) const {
  return this->singleStepValue;
}

std::vector<llvm::Instruction *>
InductionVariable::getComputationOfStepValue(void) const {
  return this->computationOfStepValue;
}

bool InductionVariable::isStepValueLoopInvariant(void) const {
  return this->isComputedStepValueLoopInvariant;
}

bool InductionVariable::isStepValuePositive(void) const {
  assert(this->isComputedStepValueLoopInvariant);
  auto *stepValue = this->getSingleComputedStepValue();
  if (this->loopEntryPHIType->isIntegerTy()) {
    auto constantValue = llvm::cast<llvm::ConstantInt>(stepValue)->getValue();
    return constantValue.isStrictlyPositive();
  }

  auto fpValue = llvm::cast<llvm::ConstantFP>(stepValue)->getValueAPF();
  return fpValue.isNonZero() && !fpValue.isNegative();
}

const llvm::SCEV *InductionVariable::getStepSCEV(void) const {
  return this->stepSCEV;
}

llvm::Type *InductionVariable::getType(void) const {
  return this->loopEntryPHIType;
}

bool InductionVariable::isIVInstruction(llvm::Instruction *inst) const {
  return this->instructions.count(inst) != 0;
}

bool InductionVariable::isDerivedFromIVInstructions(
    llvm::Instruction *inst) const {
  return this->derivedInstructions.count(inst) != 0;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
