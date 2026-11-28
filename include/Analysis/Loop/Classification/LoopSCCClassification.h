/** @file LoopSCCClassification.h @brief Semantic SCC attribute hierarchy for classified loop components. */
/*
 * Copyright 2026 Lotus contributors
 */
#pragma once

#include "Analysis/Loop/Classification/LoopCarriedVariable.h"
#include "Analysis/Loop/Dependence/LoopSCCDAG.h"
#include "Analysis/Loop/LoopStructure.h"
#include "llvm/IR/Instruction.h"

#include <set>

namespace llvm {
class AllocaInst;
class PHINode;
class Value;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace loop {

class ClonableMemoryObject;
class InductionVariable;

class GenericSCC {
public:
  enum SCCKind {
    LOOP_CARRIED,
    REDUCTION,
    BINARY_REDUCTION,
    RECOMPUTABLE,
    SINGLE_ACCUMULATOR_RECOMPUTABLE,
    INDUCTION_VARIABLE,
    LINEAR_INDUCTION_VARIABLE,
    PERIODIC_VARIABLE,
    UNKNOWN_CLOSED_FORM,
    MEMORY_CLONABLE,
    STACK_OBJECT_CLONABLE,
    LOOP_CARRIED_UNKNOWN,
    LOOP_ITERATION
  };

  GenericSCC(SCCKind K, LoopSCC *s, LoopStructure *loop);
  virtual ~GenericSCC() = default;

  LoopSCC *getSCC(void) const;
  SCCKind getKind(void) const;
  bool doesHaveMemoryDependencesWithin(void) const;
  std::set<llvm::PHINode *> getPHIs(void) const;

protected:
  LoopStructure *loop;
  LoopSCC *scc;
  std::set<llvm::PHINode *> PHINodes;

private:
  SCCKind kind;
  bool hasMemoryDependences;
};

class LoopIterationSCC : public GenericSCC {
public:
  LoopIterationSCC(LoopSCC *s, LoopStructure *loop);
};

class LoopCarriedSCC : public GenericSCC {
public:
  LoopCarriedSCC(SCCKind K,
                 LoopSCC *s,
                 LoopStructure *loop,
                 const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                 bool commutative);

  std::set<LoopDependenceEdge *> getLoopCarriedDependences(void) const;
  bool isCommutative(void) const;

protected:
  std::set<LoopDependenceEdge *> lcDeps;
  bool commutative;
};

class ReductionSCC : public LoopCarriedSCC {
public:
  ReductionSCC(SCCKind K,
               LoopSCC *s,
               LoopStructure *loop,
               const std::set<LoopDependenceEdge *> &loopCarriedDependences,
               llvm::Value *initialValue,
               llvm::PHINode *accumulator,
               llvm::Value *identity);

  llvm::Value *getInitialValue(void) const;
  llvm::Value *getIdentityValue(void) const;
  llvm::PHINode *getPhiThatAccumulatesValuesBetweenLoopIterations(void) const;

protected:
  llvm::Value *initialValue;
  llvm::PHINode *accumulator;
  llvm::Value *identity;
};

class BinaryReductionSCC : public ReductionSCC {
public:
  BinaryReductionSCC(LoopSCC *s,
                     LoopStructure *loop,
                     const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                     const LoopCarriedVariable *variable);

  llvm::Instruction::BinaryOps getReductionOperation(void) const;

private:
  llvm::Instruction::BinaryOps reductionOperation;
};

class RecomputableSCC : public LoopCarriedSCC {
public:
  RecomputableSCC(SCCKind K,
                  LoopSCC *s,
                  LoopStructure *loop,
                  const std::set<LoopDependenceEdge *> &loopCarriedDependences,
                  const std::set<llvm::Instruction *> &values,
                  bool commutative);

  std::set<llvm::Instruction *>
  getValuesToPropagateAcrossLoopIterations(void) const;

protected:
  std::set<llvm::Instruction *> values;
};

class SingleAccumulatorRecomputableSCC : public RecomputableSCC {
public:
  SingleAccumulatorRecomputableSCC(
      SCCKind K,
      LoopSCC *s,
      LoopStructure *loop,
      const std::set<LoopDependenceEdge *> &loopCarriedDependences,
      llvm::PHINode *accumulator);

  llvm::PHINode *getPhiThatAccumulatesValuesBetweenLoopIterations(void) const;

protected:
  llvm::PHINode *accumulator;
};

class InductionVariableSCC : public SingleAccumulatorRecomputableSCC {
public:
  InductionVariableSCC(
      SCCKind K,
      LoopSCC *s,
      LoopStructure *loop,
      const std::set<LoopDependenceEdge *> &loopCarriedDependences,
      llvm::PHINode *accumulator);
};

class LinearInductionVariableSCC : public InductionVariableSCC {
public:
  LinearInductionVariableSCC(
      LoopSCC *s,
      LoopStructure *loop,
      const std::set<LoopDependenceEdge *> &loopCarriedDependences,
      const std::set<InductionVariable *> &ivs);

  std::set<InductionVariable *> getIVs(void) const;

private:
  std::set<InductionVariable *> IVs;
};

class PeriodicVariableSCC : public SingleAccumulatorRecomputableSCC {
public:
  PeriodicVariableSCC(
      LoopSCC *s,
      LoopStructure *loop,
      const std::set<LoopDependenceEdge *> &loopCarriedDependences,
      llvm::Value *initialValue,
      llvm::Value *period,
      llvm::Value *step,
      llvm::PHINode *accumulator);

  llvm::Value *getInitialValue(void) const;
  llvm::Value *getPeriod(void) const;
  llvm::Value *getStepValue(void) const;

private:
  llvm::Value *initialValue;
  llvm::Value *period;
  llvm::Value *step;
};

class UnknownClosedFormSCC : public RecomputableSCC {
public:
  UnknownClosedFormSCC(
      LoopSCC *s,
      LoopStructure *loop,
      const std::set<LoopDependenceEdge *> &loopCarriedDependences,
      const std::set<llvm::Instruction *> &values);
};

class MemoryClonableSCC : public LoopCarriedSCC {
public:
  MemoryClonableSCC(
      SCCKind K,
      LoopSCC *s,
      LoopStructure *loop,
      const std::set<LoopDependenceEdge *> &loopCarriedDependences);
};

class StackObjectClonableSCC : public MemoryClonableSCC {
public:
  StackObjectClonableSCC(
      LoopSCC *s,
      LoopStructure *loop,
      const std::set<LoopDependenceEdge *> &loopCarriedDependences,
      const std::set<ClonableMemoryObject *> &locations);

  std::set<llvm::AllocaInst *> getMemoryLocationsToClone(void) const;
  std::set<ClonableMemoryObject *> getClonableMemoryObjects(void) const;

private:
  std::set<ClonableMemoryObject *> locations;
};

class LoopCarriedUnknownSCC : public LoopCarriedSCC {
public:
  LoopCarriedUnknownSCC(
      LoopSCC *s,
      LoopStructure *loop,
      const std::set<LoopDependenceEdge *> &loopCarriedDependences);
};

} // namespace loop
} // namespace analysis
} // namespace lotus
