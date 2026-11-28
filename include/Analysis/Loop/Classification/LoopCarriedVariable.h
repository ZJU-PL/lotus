/** @file LoopCarriedVariable.h @brief Loop-carried reduction variable modeling and algebraic updatability checks. */
/*
 * Copyright 2026 Lotus contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma once

#include "Analysis/Loop/Dependence/LoopCarriedDependencies.h"
#include "llvm/IR/Instruction.h"

#include <memory>
#include <set>
#include <unordered_set>
#include <vector>

namespace llvm {
class CastInst;
class PHINode;
class Use;
class Value;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace loop {

class EvolutionUpdate {
public:
  EvolutionUpdate(llvm::Instruction *updateInstruction,
                  const std::unordered_set<llvm::Value *> &internalVariableValues);

  bool mayUpdateBeOverride(void) const;
  bool isCommutativeWithSelf(void) const;
  bool isAssociativeWithSelf(void) const;
  bool isTransformablyCommutativeWithSelf(void) const;
  bool isTransformablyCommutativeWith(const EvolutionUpdate &other) const;
  bool isAssociativeWith(const EvolutionUpdate &other) const;
  llvm::Instruction *getUpdateInstruction(void) const;
  bool isAdd(void) const;
  bool isMul(void) const;
  bool isSub(void) const;
  bool isSubTransformableToAdd(void) const;

private:
  bool isBothUpdatesAddOrSub(const EvolutionUpdate &other) const;
  bool isBothUpdatesMul(const EvolutionUpdate &other) const;
  bool isBothUpdatesSameBitwiseLogicalOp(const EvolutionUpdate &other) const;

  llvm::Instruction *updateInstruction;
  llvm::Value *newValue;
  std::unordered_set<llvm::Use *> internalValuesUsed;
  std::unordered_set<llvm::Use *> externalValuesUsed;
};

class LoopCarriedVariable {
public:
  LoopCarriedVariable(const LoopStructure &loop,
                      LoopTree *loopNode,
                      LoopDependenceGraph &loopDG,
                      LoopSCCDAG &sccdag,
                      LoopSCC &variableSCC,
                      llvm::PHINode *declarationPHI);

  bool isEvolutionReducibleAcrossLoopIterations(void) const;
  llvm::PHINode *getLoopEntryPHIForValueOfVariable(llvm::Value *value) const;
  llvm::Value *getInitialValue(void) const;
  llvm::Instruction::BinaryOps getReductionOperation(void) const;
  llvm::PHINode *getAccumulator(void) const;
  llvm::Value *getIdentityValue(void) const;

private:
  std::unordered_set<llvm::Value *> produceDataAndMemoryOnlySCC(
      LoopDependenceGraph &loopDG,
      const std::unordered_set<LoopDependenceEdge *>
          &loopCarriedDependenciesNotOfVariable) const;
  void collectControlValuesGoverningEvolution(
      LoopDependenceGraph &loopDG,
      const std::unordered_set<LoopDependenceEdge *>
          &loopCarriedDependenciesNotOfVariable);
  bool collectVariableUpdates(
      const std::unordered_set<llvm::Value *> &loopCarriedValues);
  std::unordered_set<llvm::Value *> getConsumersOfVariable(void) const;
  bool areValuesPropagatingVariableIntermediatesOutsideLoop(
      const std::unordered_set<llvm::Value *> &values) const;
  bool hasRoundingError(
      const std::unordered_set<EvolutionUpdate *> &arithmeticUpdates) const;

  bool isValid{false};
  const LoopStructure &outermostLoopOfVariable;
  llvm::PHINode *declarationPHI;
  std::unordered_set<llvm::Value *> sccOfVariableOnlyValues;
  std::unordered_set<llvm::Value *> sccOfDataAndMemoryVariableValuesOnly;
  std::set<llvm::Value *> controlValuesGoverningEvolution;
  std::vector<std::unique_ptr<EvolutionUpdate>> ownedVariableUpdates;
  std::unordered_set<EvolutionUpdate *> variableUpdates;
  std::unordered_set<EvolutionUpdate *> loopCarriedVariableUpdates;
  std::unordered_set<llvm::CastInst *> castsInternalToVariableComputation;
  llvm::Value *initialValue{nullptr};
  mutable llvm::PHINode *accumulator{nullptr};
  mutable llvm::Instruction::BinaryOps reductionOperation{
      llvm::Instruction::BinaryOpsEnd};
  mutable llvm::Value *identityValue{nullptr};
};

} // namespace loop
} // namespace analysis
} // namespace lotus
