/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Classification/LoopCarriedVariable.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"

#include <cassert>
#include <functional>
#include <queue>

namespace lotus {
namespace analysis {
namespace loop {

namespace {

std::unordered_set<llvm::Value *> computeSCCContainingValue(
    LoopDependenceGraph &loopDG,
    const std::unordered_set<llvm::Value *> &candidateValues,
    llvm::Value *seed,
    const std::function<bool(LoopDependenceEdge *)> &isEdgeAllowed) {
  if (seed == nullptr || candidateValues.count(seed) == 0) {
    return {};
  }

  auto collectReachable = [&](bool forward) {
    std::unordered_set<llvm::Value *> reachable;
    std::queue<llvm::Value *> worklist;
    reachable.insert(seed);
    worklist.push(seed);

    while (!worklist.empty()) {
      auto *value = worklist.front();
      worklist.pop();

      auto *node = loopDG.getNode(value);
      if (node == nullptr) {
        continue;
      }

      const auto &edges =
          forward ? node->getOutgoingEdges() : node->getIncomingEdges();
      for (auto *edge : edges) {
        if (edge == nullptr || !isEdgeAllowed(edge)) {
          continue;
        }

        auto *nextNode = forward ? edge->getDst() : edge->getSrc();
        if (nextNode == nullptr) {
          continue;
        }

        auto *nextValue = nextNode->getValue();
        if (nextValue == nullptr || candidateValues.count(nextValue) == 0) {
          continue;
        }

        if (reachable.insert(nextValue).second) {
          worklist.push(nextValue);
        }
      }
    }

    return reachable;
  };

  auto forwardReachable = collectReachable(true);
  auto reverseReachable = collectReachable(false);

  std::unordered_set<llvm::Value *> scc;
  for (auto *value : forwardReachable) {
    if (reverseReachable.count(value) != 0) {
      scc.insert(value);
    }
  }
  return scc;
}

llvm::Value *getIdentityValueForReduction(llvm::Type *type,
                                          llvm::Instruction::BinaryOps op) {
  if (type == nullptr) {
    return nullptr;
  }

  switch (op) {
  case llvm::Instruction::Add:
  case llvm::Instruction::FAdd:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
    return llvm::Constant::getNullValue(type);
  case llvm::Instruction::Mul:
    return type->isIntegerTy() ? llvm::ConstantInt::get(type, 1) : nullptr;
  case llvm::Instruction::FMul:
    return type->isFloatingPointTy() ? llvm::ConstantFP::get(type, 1.0)
                                     : nullptr;
  case llvm::Instruction::And:
    return type->isIntegerTy() ? llvm::Constant::getAllOnesValue(type)
                               : nullptr;
  default:
    return nullptr;
  }
}

llvm::Instruction::BinaryOps getReductionOperationForType(
    llvm::Type *type,
    llvm::Instruction::BinaryOps op) {
  if (type == nullptr) {
    return llvm::Instruction::BinaryOpsEnd;
  }

  switch (op) {
  case llvm::Instruction::Sub:
    if (type->isIntegerTy()) {
      return llvm::Instruction::Add;
    }
    if (type->isFloatingPointTy()) {
      return llvm::Instruction::FAdd;
    }
    return llvm::Instruction::BinaryOpsEnd;
  case llvm::Instruction::FSub:
    return type->isFloatingPointTy() ? llvm::Instruction::FAdd
                                     : llvm::Instruction::BinaryOpsEnd;
  default:
    return op;
  }
}

} // namespace

EvolutionUpdate::EvolutionUpdate(
    llvm::Instruction *updateInstruction,
    const std::unordered_set<llvm::Value *> &internalVariableValues)
    : updateInstruction{updateInstruction}, newValue{nullptr} {
  if (auto *storeUpdate = llvm::dyn_cast<llvm::StoreInst>(updateInstruction)) {
    this->newValue = storeUpdate->getValueOperand();
    return;
  }

  this->newValue = updateInstruction;
  for (auto &use : updateInstruction->operands()) {
    auto *usedValue = use.get();
    if (internalVariableValues.count(usedValue) != 0) {
      this->internalValuesUsed.insert(&use);
    } else {
      this->externalValuesUsed.insert(&use);
    }
  }
}

bool EvolutionUpdate::mayUpdateBeOverride(void) const {
  if (llvm::isa<llvm::SelectInst>(this->updateInstruction) ||
      llvm::isa<llvm::PHINode>(this->updateInstruction)) {
    return !this->externalValuesUsed.empty();
  }
  if (llvm::isa<llvm::CallInst>(this->updateInstruction)) {
    return true;
  }
  if (this->updateInstruction->isBinaryOp()) {
    return this->internalValuesUsed.empty();
  }
  if (llvm::isa<llvm::CmpInst>(this->updateInstruction)) {
    for (auto *user : this->updateInstruction->users()) {
      if (!llvm::isa<llvm::SelectInst>(user)) {
        return true;
      }
    }
    return false;
  }
  return true;
}

bool EvolutionUpdate::isCommutativeWithSelf(void) const {
  if (this->mayUpdateBeOverride()) {
    return false;
  }
  return this->updateInstruction->isCommutative();
}

bool EvolutionUpdate::isAssociativeWithSelf(void) const {
  if (this->mayUpdateBeOverride()) {
    return false;
  }
  if (this->updateInstruction->isAssociative() || this->isAdd() ||
      this->isMul()) {
    return true;
  }
  return this->isSubTransformableToAdd();
}

bool EvolutionUpdate::isTransformablyCommutativeWithSelf(void) const {
  if (this->mayUpdateBeOverride()) {
    return false;
  }
  if (this->updateInstruction->isCommutative()) {
    return true;
  }
  return this->isSubTransformableToAdd();
}

bool EvolutionUpdate::isTransformablyCommutativeWith(
    const EvolutionUpdate &other) const {
  if (!this->isTransformablyCommutativeWithSelf() ||
      !other.isTransformablyCommutativeWithSelf()) {
    return false;
  }
  return this->isBothUpdatesAddOrSub(other) || this->isBothUpdatesMul(other) ||
         this->isBothUpdatesSameBitwiseLogicalOp(other);
}

bool EvolutionUpdate::isAssociativeWith(const EvolutionUpdate &other) const {
  if (!this->isAssociativeWithSelf() || !other.isAssociativeWithSelf()) {
    return false;
  }
  return this->isBothUpdatesAddOrSub(other) || this->isBothUpdatesMul(other) ||
         this->isBothUpdatesSameBitwiseLogicalOp(other);
}

llvm::Instruction *EvolutionUpdate::getUpdateInstruction(void) const {
  return this->updateInstruction;
}

bool EvolutionUpdate::isAdd(void) const {
  auto op = this->updateInstruction->getOpcode();
  return op == llvm::Instruction::Add || op == llvm::Instruction::FAdd;
}

bool EvolutionUpdate::isMul(void) const {
  auto op = this->updateInstruction->getOpcode();
  return op == llvm::Instruction::Mul || op == llvm::Instruction::FMul;
}

bool EvolutionUpdate::isSub(void) const {
  auto op = this->updateInstruction->getOpcode();
  return op == llvm::Instruction::Sub || op == llvm::Instruction::FSub;
}

bool EvolutionUpdate::isSubTransformableToAdd(void) const {
  if (!this->isSub()) {
    return false;
  }
  auto &useOfValueBeingSubtracted = this->updateInstruction->getOperandUse(1);
  return this->externalValuesUsed.find(&useOfValueBeingSubtracted) !=
         this->externalValuesUsed.end();
}

bool EvolutionUpdate::isBothUpdatesAddOrSub(
    const EvolutionUpdate &other) const {
  bool isThisAddOrSub = this->isAdd() || this->isSubTransformableToAdd();
  bool isOtherAddOrSub = other.isAdd() || other.isSubTransformableToAdd();
  return isThisAddOrSub && isOtherAddOrSub;
}

bool EvolutionUpdate::isBothUpdatesMul(const EvolutionUpdate &other) const {
  return this->isMul() && other.isMul();
}

bool EvolutionUpdate::isBothUpdatesSameBitwiseLogicalOp(
    const EvolutionUpdate &other) const {
  auto thisOp = this->updateInstruction->getOpcode();
  auto otherOp = other.updateInstruction->getOpcode();
  return this->updateInstruction->isBitwiseLogicOp() &&
         other.updateInstruction->isBitwiseLogicOp() && thisOp == otherOp;
}

LoopCarriedVariable::LoopCarriedVariable(const LoopStructure &loop,
                                         LoopTree *loopNode,
                                         LoopDependenceGraph &loopDG,
                                         LoopSCCDAG &sccdag,
                                         LoopSCC &variableSCC,
                                         llvm::PHINode *declarationPHI)
    : outermostLoopOfVariable{loop}, declarationPHI{declarationPHI} {
  assert(variableSCC.isInternal(declarationPHI));

  auto *preHeader = this->outermostLoopOfVariable.getPreHeader();
  if (preHeader == nullptr ||
      declarationPHI->getBasicBlockIndex(preHeader) < 0) {
    return;
  }
  this->initialValue = declarationPHI->getIncomingValueForBlock(preHeader);

  auto loopCarriedDependencies =
      LoopCarriedDependencies::getLoopCarriedDependenciesForLoop(loop, loopNode,
                                                                 sccdag);
  std::unordered_set<llvm::Value *> loopCarriedValues;
  std::unordered_set<LoopDependenceEdge *> loopCarriedDependenciesNotOfVariable;
  for (auto *dependency : loopCarriedDependencies) {
    auto *consumer = dependency->getDst()->getValue();
    if (consumer == declarationPHI) {
      loopCarriedValues.insert(dependency->getSrc()->getValue());
    } else {
      loopCarriedDependenciesNotOfVariable.insert(dependency);
    }
  }

  std::unordered_set<llvm::Value *> allPossibleInternalValues;
  for (const auto &nodePair : variableSCC.internalNodePairs()) {
    if (nodePair.first != nullptr) {
      allPossibleInternalValues.insert(nodePair.first);
    }
  }

  auto variableEdgeIsAllowed = [&](LoopDependenceEdge *edge) -> bool {
    return loopCarriedDependenciesNotOfVariable.count(edge) == 0;
  };
  this->sccOfVariableOnlyValues =
      computeSCCContainingValue(loopDG, allPossibleInternalValues,
                                this->declarationPHI, variableEdgeIsAllowed);
  if (this->sccOfVariableOnlyValues.empty()) {
    return;
  }

  this->sccOfDataAndMemoryVariableValuesOnly =
      this->produceDataAndMemoryOnlySCC(loopDG,
                                        loopCarriedDependenciesNotOfVariable);
  if (this->sccOfDataAndMemoryVariableValuesOnly.empty()) {
    return;
  }

  this->collectControlValuesGoverningEvolution(
      loopDG, loopCarriedDependenciesNotOfVariable);
  if (!this->collectVariableUpdates(loopCarriedValues)) {
    return;
  }

  this->accumulator = this->declarationPHI;
  this->isValid = true;
}

std::unordered_set<llvm::Value *>
LoopCarriedVariable::produceDataAndMemoryOnlySCC(
    LoopDependenceGraph &loopDG,
    const std::unordered_set<LoopDependenceEdge *>
        &loopCarriedDependenciesNotOfVariable) const {
  std::unordered_set<llvm::Value *> values;
  for (auto *value : this->sccOfVariableOnlyValues) {
    auto *node = loopDG.getNode(value);
    if (node == nullptr) {
      continue;
    }
    bool producesControlDependency = false;
    for (auto *edge : node->getOutgoingEdges()) {
      if (loopCarriedDependenciesNotOfVariable.count(edge) != 0) {
        continue;
      }
      auto *dst = edge->getDst();
      if (dst == nullptr ||
          this->sccOfVariableOnlyValues.count(dst->getValue()) == 0) {
        continue;
      }
      if (edge->getKind() == LoopDependenceEdgeKind::Control) {
        producesControlDependency = true;
        break;
      }
    }
    if (!producesControlDependency) {
      values.insert(value);
    }
  }

  if (values.count(this->declarationPHI) == 0) {
    return {};
  }

  auto dataMemoryEdgeIsAllowed = [&](LoopDependenceEdge *edge) -> bool {
    return loopCarriedDependenciesNotOfVariable.count(edge) == 0 &&
           edge->getKind() != LoopDependenceEdgeKind::Control;
  };

  return computeSCCContainingValue(loopDG, values, this->declarationPHI,
                                   dataMemoryEdgeIsAllowed);
}

void LoopCarriedVariable::collectControlValuesGoverningEvolution(
    LoopDependenceGraph &loopDG,
    const std::unordered_set<LoopDependenceEdge *>
        &loopCarriedDependenciesNotOfVariable) {
  for (auto *value : this->sccOfVariableOnlyValues) {
    if (auto *selectInst = llvm::dyn_cast<llvm::SelectInst>(value)) {
      this->controlValuesGoverningEvolution.insert(selectInst->getCondition());
      continue;
    }

    auto *node = loopDG.getNode(value);
    if (node == nullptr) {
      continue;
    }
    for (auto *edge : node->getOutgoingEdges()) {
      if (loopCarriedDependenciesNotOfVariable.count(edge) != 0) {
        continue;
      }
      auto *dst = edge->getDst();
      if (dst == nullptr ||
          this->sccOfVariableOnlyValues.count(dst->getValue()) == 0) {
        continue;
      }
      if (edge->getKind() == LoopDependenceEdgeKind::Control) {
        this->controlValuesGoverningEvolution.insert(value);
        break;
      }
    }
  }
}

bool LoopCarriedVariable::collectVariableUpdates(
    const std::unordered_set<llvm::Value *> &loopCarriedValues) {
  for (auto *value : this->sccOfDataAndMemoryVariableValuesOnly) {
    if (value == this->declarationPHI) {
      continue;
    }
    auto *inst = llvm::dyn_cast<llvm::Instruction>(value);
    if (inst == nullptr) {
      return false;
    }
    if (llvm::isa<llvm::LoadInst>(inst)) {
      continue;
    }
    if (auto *castInst = llvm::dyn_cast<llvm::CastInst>(inst)) {
      this->castsInternalToVariableComputation.insert(castInst);
      continue;
    }

    auto ownedUpdate = std::make_unique<EvolutionUpdate>(
        inst, this->sccOfDataAndMemoryVariableValuesOnly);
    auto *update = ownedUpdate.get();
    this->ownedVariableUpdates.push_back(std::move(ownedUpdate));
    this->variableUpdates.insert(update);
    if (loopCarriedValues.count(value) != 0) {
      this->loopCarriedVariableUpdates.insert(update);
    }
  }
  return true;
}

bool LoopCarriedVariable::isEvolutionReducibleAcrossLoopIterations(
    void) const {
  if (!this->isValid) {
    return false;
  }

  for (auto *controlValue : this->controlValuesGoverningEvolution) {
    if (this->sccOfVariableOnlyValues.count(controlValue) != 0) {
      return false;
    }
  }

  std::unordered_set<EvolutionUpdate *> arithmeticUpdates;
  for (auto *update : this->variableUpdates) {
    if (update->mayUpdateBeOverride()) {
      return false;
    }
    auto *updateInstruction = update->getUpdateInstruction();
    if (llvm::isa<llvm::PHINode>(updateInstruction) ||
        llvm::isa<llvm::SelectInst>(updateInstruction)) {
      continue;
    }
    arithmeticUpdates.insert(update);
  }

  if (this->hasRoundingError(arithmeticUpdates) || arithmeticUpdates.empty()) {
    return false;
  }

  for (auto *update : arithmeticUpdates) {
    for (auto *otherUpdate : arithmeticUpdates) {
      if (!update->isTransformablyCommutativeWith(*otherUpdate) ||
          !update->isAssociativeWith(*otherUpdate)) {
        return false;
      }
    }
  }

  auto consumers = this->getConsumersOfVariable();
  if (!this->areValuesPropagatingVariableIntermediatesOutsideLoop(consumers)) {
    return false;
  }

  auto operation = llvm::Instruction::BinaryOpsEnd;
  for (auto *update : arithmeticUpdates) {
    auto currentOperation = getReductionOperationForType(
        this->declarationPHI->getType(),
        static_cast<llvm::Instruction::BinaryOps>(
            update->getUpdateInstruction()->getOpcode()));
    if (currentOperation == llvm::Instruction::BinaryOpsEnd) {
      return false;
    }
    if (operation == llvm::Instruction::BinaryOpsEnd) {
      operation = currentOperation;
      continue;
    }
    if (operation != currentOperation) {
      return false;
    }
  }

  auto *identity =
      getIdentityValueForReduction(this->declarationPHI->getType(), operation);
  if (identity == nullptr) {
    return false;
  }

  this->reductionOperation = operation;
  this->identityValue = identity;
  return true;
}

std::unordered_set<llvm::Value *>
LoopCarriedVariable::getConsumersOfVariable(void) const {
  std::unordered_set<llvm::Value *> consumers;
  for (auto *value : this->sccOfVariableOnlyValues) {
    for (auto *user : value->users()) {
      auto *consumer = llvm::dyn_cast<llvm::Instruction>(user);
      if (consumer == nullptr ||
          !this->outermostLoopOfVariable.isIncluded(consumer) ||
          this->sccOfVariableOnlyValues.count(consumer) != 0) {
        continue;
      }
      consumers.insert(consumer);
    }
  }
  return consumers;
}

bool LoopCarriedVariable::areValuesPropagatingVariableIntermediatesOutsideLoop(
    const std::unordered_set<llvm::Value *> &values) const {
  auto *loopHeader = this->outermostLoopOfVariable.getHeader();
  auto *loopPreheader = this->outermostLoopOfVariable.getPreHeader();
  std::queue<llvm::Value *> valuesToCheck;
  std::unordered_set<llvm::Value *> valuesChecked;
  for (auto *value : values) {
    valuesToCheck.push(value);
    valuesChecked.insert(value);
  }

  while (!valuesToCheck.empty()) {
    auto *value = valuesToCheck.front();
    valuesToCheck.pop();

    if (auto *castInst = llvm::dyn_cast<llvm::CastInst>(value)) {
      if (this->sccOfDataAndMemoryVariableValuesOnly.count(
              castInst->getOperand(0)) == 0) {
        return false;
      }
    } else if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value)) {
      if (loopHeader != phi->getParent()) {
        return false;
      }

      llvm::Value *singleIncomingValue = nullptr;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        auto *incomingBlock = phi->getIncomingBlock(i);
        auto *incomingValue = phi->getIncomingValue(i);
        if (incomingBlock == loopPreheader) {
          continue;
        }
        if (singleIncomingValue == nullptr ||
            incomingValue == singleIncomingValue) {
          singleIncomingValue = incomingValue;
          continue;
        }
        singleIncomingValue = nullptr;
        break;
      }

      if (singleIncomingValue == nullptr ||
          this->sccOfDataAndMemoryVariableValuesOnly.count(
              singleIncomingValue) == 0) {
        return false;
      }
    } else {
      return false;
    }

    for (auto *user : value->users()) {
      auto *userI = llvm::dyn_cast<llvm::Instruction>(user);
      if (userI == nullptr ||
          !this->outermostLoopOfVariable.isIncluded(userI->getParent()) ||
          !valuesChecked.insert(userI).second) {
        continue;
      }
      valuesToCheck.push(userI);
    }
  }

  return true;
}

llvm::PHINode *LoopCarriedVariable::getLoopEntryPHIForValueOfVariable(
    llvm::Value *value) const {
  if (this->sccOfVariableOnlyValues.count(value) == 0) {
    return llvm::dyn_cast<llvm::PHINode>(value);
  }
  return this->declarationPHI;
}

bool LoopCarriedVariable::hasRoundingError(
    const std::unordered_set<EvolutionUpdate *> &arithmeticUpdates) const {
  bool isIntegerTypedCast = false;
  bool isFloatingPointTypedCast = false;
  for (auto *castInst : this->castsInternalToVariableComputation) {
    auto *castTy = castInst->getType();
    isIntegerTypedCast |= castTy->isIntegerTy();
    isFloatingPointTypedCast |= castTy->isFloatingPointTy();

    auto *srcType = castInst->getSrcTy();
    if (castTy->isFloatingPointTy() && srcType->isFloatingPointTy()) {
      return true;
    }
  }
  if (!isIntegerTypedCast || !isFloatingPointTypedCast) {
    return false;
  }

  auto *accumulationType = this->declarationPHI->getType();
  bool onlyAddition = true;
  for (auto *update : arithmeticUpdates) {
    onlyAddition &= update->isAdd() || update->isSubTransformableToAdd();
  }
  return !(accumulationType->isIntegerTy() && onlyAddition);
}

llvm::Value *LoopCarriedVariable::getInitialValue(void) const {
  return this->initialValue;
}

llvm::Instruction::BinaryOps LoopCarriedVariable::getReductionOperation(
    void) const {
  return this->reductionOperation;
}

llvm::PHINode *LoopCarriedVariable::getAccumulator(void) const {
  return this->accumulator;
}

llvm::Value *LoopCarriedVariable::getIdentityValue(void) const {
  return this->identityValue;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
