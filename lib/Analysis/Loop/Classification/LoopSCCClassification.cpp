/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/Classification/LoopSCCClassification.h"

#include "Analysis/Loop/Memory/MemoryCloningAnalysis.h"
#include "Analysis/Loop/Scalar/InductionVariable.h"
#include "llvm/IR/Instructions.h"

namespace lotus {
namespace analysis {
namespace loop {

GenericSCC::GenericSCC(SCCKind K, LoopSCC *s, LoopStructure *loop)
    : loop{loop}, scc{s}, kind{K}, hasMemoryDependences{false} {
  std::unordered_set<LoopDependenceNode *> internalNodeSet;
  for (const auto &pair : s->internalNodePairs()) {
    auto *node = pair.second;
    internalNodeSet.insert(node);
    if (auto *phi = llvm::dyn_cast_or_null<llvm::PHINode>(node->getValue())) {
      this->PHINodes.insert(phi);
    }
  }

  for (const auto &pair : s->internalNodePairs()) {
    auto *node = pair.second;
    for (auto *edge : node->getOutgoingEdges()) {
      auto *dst = edge->getDst();
      if (dst == nullptr || internalNodeSet.count(dst) == 0) {
        continue;
      }
      if (edge->getKind() == LoopDependenceEdgeKind::Memory) {
        this->hasMemoryDependences = true;
      }
    }
  }
}

LoopSCC *GenericSCC::getSCC(void) const { return this->scc; }
GenericSCC::SCCKind GenericSCC::getKind(void) const { return this->kind; }
bool GenericSCC::doesHaveMemoryDependencesWithin(void) const {
  return this->hasMemoryDependences;
}
std::set<llvm::PHINode *> GenericSCC::getPHIs(void) const {
  return this->PHINodes;
}

LoopIterationSCC::LoopIterationSCC(LoopSCC *s, LoopStructure *loop)
    : GenericSCC(LOOP_ITERATION, s, loop) {}

LoopCarriedSCC::LoopCarriedSCC(
    SCCKind K,
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    bool commutative)
    : GenericSCC(K, s, loop), lcDeps{loopCarriedDependences},
      commutative{commutative} {}

std::set<LoopDependenceEdge *>
LoopCarriedSCC::getLoopCarriedDependences(void) const {
  return this->lcDeps;
}

bool LoopCarriedSCC::isCommutative(void) const { return this->commutative; }

ReductionSCC::ReductionSCC(
    SCCKind K,
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    llvm::Value *initialValue,
    llvm::PHINode *accumulator,
    llvm::Value *identity)
    : LoopCarriedSCC(K, s, loop, loopCarriedDependences, true),
      initialValue{initialValue}, accumulator{accumulator},
      identity{identity} {}

llvm::Value *ReductionSCC::getInitialValue(void) const {
  return this->initialValue;
}

llvm::Value *ReductionSCC::getIdentityValue(void) const {
  return this->identity;
}

llvm::PHINode *
ReductionSCC::getPhiThatAccumulatesValuesBetweenLoopIterations(void) const {
  return this->accumulator;
}

BinaryReductionSCC::BinaryReductionSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    const LoopCarriedVariable *variable)
    : ReductionSCC(BINARY_REDUCTION, s, loop, loopCarriedDependences,
                   variable->getInitialValue(), variable->getAccumulator(),
                   variable->getIdentityValue()),
      reductionOperation{variable->getReductionOperation()} {}

llvm::Instruction::BinaryOps
BinaryReductionSCC::getReductionOperation(void) const {
  return this->reductionOperation;
}

RecomputableSCC::RecomputableSCC(
    SCCKind K,
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    const std::set<llvm::Instruction *> &values,
    bool commutative)
    : LoopCarriedSCC(K, s, loop, loopCarriedDependences, commutative),
      values{values} {}

std::set<llvm::Instruction *>
RecomputableSCC::getValuesToPropagateAcrossLoopIterations(void) const {
  return this->values;
}

SingleAccumulatorRecomputableSCC::SingleAccumulatorRecomputableSCC(
    SCCKind K,
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    llvm::PHINode *accumulator)
    : RecomputableSCC(K, s, loop, loopCarriedDependences, {}, true),
      accumulator{accumulator} {}

llvm::PHINode *SingleAccumulatorRecomputableSCC::
    getPhiThatAccumulatesValuesBetweenLoopIterations(void) const {
  return this->accumulator;
}

InductionVariableSCC::InductionVariableSCC(
    SCCKind K,
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    llvm::PHINode *accumulator)
    : SingleAccumulatorRecomputableSCC(K, s, loop, loopCarriedDependences,
                                       accumulator) {}

LinearInductionVariableSCC::LinearInductionVariableSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    const std::set<InductionVariable *> &ivs)
    : InductionVariableSCC(
          LINEAR_INDUCTION_VARIABLE, s, loop, loopCarriedDependences,
          ivs.empty() ? nullptr : (*ivs.begin())->getLoopEntryPHI()),
      IVs{ivs} {}

std::set<InductionVariable *> LinearInductionVariableSCC::getIVs(void) const {
  return this->IVs;
}

PeriodicVariableSCC::PeriodicVariableSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    llvm::Value *initialValue,
    llvm::Value *period,
    llvm::Value *step,
    llvm::PHINode *accumulator)
    : SingleAccumulatorRecomputableSCC(PERIODIC_VARIABLE, s, loop,
                                       loopCarriedDependences, accumulator),
      initialValue{initialValue}, period{period}, step{step} {}

llvm::Value *PeriodicVariableSCC::getInitialValue(void) const {
  return this->initialValue;
}

llvm::Value *PeriodicVariableSCC::getPeriod(void) const { return this->period; }

llvm::Value *PeriodicVariableSCC::getStepValue(void) const {
  return this->step;
}

UnknownClosedFormSCC::UnknownClosedFormSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    const std::set<llvm::Instruction *> &values)
    : RecomputableSCC(UNKNOWN_CLOSED_FORM, s, loop, loopCarriedDependences,
                      values, false) {}

MemoryClonableSCC::MemoryClonableSCC(
    SCCKind K,
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences)
    : LoopCarriedSCC(K, s, loop, loopCarriedDependences, false) {}

StackObjectClonableSCC::StackObjectClonableSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences,
    const std::set<ClonableMemoryObject *> &locations)
    : MemoryClonableSCC(STACK_OBJECT_CLONABLE, s, loop, loopCarriedDependences),
      locations{locations} {}

std::set<llvm::AllocaInst *>
StackObjectClonableSCC::getMemoryLocationsToClone(void) const {
  std::set<llvm::AllocaInst *> allocations;
  for (auto *location : this->locations) {
    if (location != nullptr) {
      allocations.insert(location->getAllocation());
    }
  }
  return allocations;
}

std::set<ClonableMemoryObject *>
StackObjectClonableSCC::getClonableMemoryObjects(void) const {
  return this->locations;
}

LoopCarriedUnknownSCC::LoopCarriedUnknownSCC(
    LoopSCC *s,
    LoopStructure *loop,
    const std::set<LoopDependenceEdge *> &loopCarriedDependences)
    : LoopCarriedSCC(LOOP_CARRIED_UNKNOWN, s, loop, loopCarriedDependences,
                     false) {}

} // namespace loop
} // namespace analysis
} // namespace lotus
