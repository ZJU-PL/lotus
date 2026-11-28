/** @file InductionVariable.h @brief Individual induction variable descriptor for loop analysis. */
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

#include "Analysis/Loop/Dependence/LoopSCCDAG.h"
#include "Analysis/Loop/LoopEnvironment.h"
#include "Analysis/Loop/LoopStructure.h"
#include "Analysis/Loop/Scalar/LoopInvariants.h"

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>

namespace llvm {
class BasicBlock;
class InductionDescriptor;
class Instruction;
class PHINode;
class SCEV;
class SCEVConstant;
class SCEVUnknown;
class ScalarEvolution;
class Type;
class Value;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace loop {

namespace detail {
class ScalarEvolutionReferentialExpander;
} // namespace detail

class InductionVariable {
public:
  struct CollectedInstructions {
    std::unordered_set<llvm::PHINode *> phis;
    std::unordered_set<llvm::Instruction *> nonPHIInstructions;
    std::unordered_set<llvm::Instruction *> instructions;
  };

  InductionVariable(LoopStructure *loop,
                    LoopSCC *scc,
                    llvm::PHINode *loopEntryPHI,
                    llvm::Value *startValue,
                    const llvm::SCEV *stepSCEV,
                    llvm::Value *singleStepValue,
                    std::unordered_set<llvm::PHINode *> stepPHIs,
                    std::unordered_set<llvm::PHINode *> phis,
                    std::unordered_set<llvm::Instruction *> nonPHIInstructions,
                    std::unordered_set<llvm::Instruction *> instructions,
                    std::unordered_set<llvm::Instruction *> derivedInstructions);

  InductionVariable(
      LoopStructure *loop,
      InvariantManager &IVM,
      llvm::ScalarEvolution &SE,
      int64_t stepMultiplier,
      llvm::PHINode *loopEntryPHI,
      std::unordered_set<llvm::PHINode *> stepPHIs,
      LoopSCC *scc,
      LoopEnvironment &loopEnvironment,
      detail::ScalarEvolutionReferentialExpander &referentialExpander);

  InductionVariable(
      LoopStructure *loop,
      InvariantManager &IVM,
      llvm::ScalarEvolution &SE,
      llvm::PHINode *loopEntryPHI,
      LoopSCC *scc,
      LoopEnvironment &loopEnvironment,
      detail::ScalarEvolutionReferentialExpander &referentialExpander,
      llvm::InductionDescriptor &descriptor);

  ~InductionVariable();

  static CollectedInstructions collectIVInstructions(
      LoopStructure &loop,
      LoopSCC *scc,
      llvm::PHINode *loopEntryPHI,
      bool includeTransitiveCasts);

  static std::unordered_set<llvm::Instruction *> collectDerivedSCEVInstructions(
      LoopStructure &loop,
      InvariantManager &IVM,
      llvm::ScalarEvolution &SE,
      const std::unordered_set<llvm::Instruction *> &ivInstructions,
      bool allowBinaryOps);

  LoopSCC *getSCC(void) const;
  llvm::PHINode *getLoopEntryPHI(void) const;
  std::unordered_set<llvm::PHINode *>
  getPHIsInvolvedInComputingIVStep(void) const;
  std::unordered_set<llvm::PHINode *> getPHIs(void) const;
  std::unordered_set<llvm::Instruction *> getNonPHIIntermediateValues(void) const;
  std::unordered_set<llvm::Instruction *> getAllInstructions(void) const;
  std::unordered_set<llvm::Instruction *> getDerivedSCEVInstructions(void) const;
  llvm::Value *getStartValue(void) const;
  llvm::Value *getSingleComputedStepValue(void) const;
  std::vector<llvm::Instruction *> getComputationOfStepValue(void) const;
  bool isStepValueLoopInvariant(void) const;
  bool isStepValuePositive(void) const;
  const llvm::SCEV *getStepSCEV(void) const;
  llvm::Type *getType(void) const;
  bool isIVInstruction(llvm::Instruction *inst) const;
  bool isDerivedFromIVInstructions(llvm::Instruction *inst) const;

private:
  void collectValuesInternalAndExternalToLoopAndSCC(
      LoopEnvironment &loopEnvironment);
  void deriveStepValue(
      llvm::ScalarEvolution &SE,
      detail::ScalarEvolutionReferentialExpander &referentialExpander,
      int64_t multiplier);
  void deriveStepValueFromSCEVConstant(const llvm::SCEVConstant *scev,
                                       int64_t multiplier);
  void deriveStepValueFromSCEVUnknown(const llvm::SCEVUnknown *scev);
  bool deriveStepValueFromCompositeSCEV(
      const llvm::SCEV *scev,
      detail::ScalarEvolutionReferentialExpander &referentialExpander);
  void traverseCycleThroughLoopEntryPHIToGetAllIVInstructions(void);
  void traverseConsumersOfIVInstructionsToGetAllDerivedSCEVInstructions(
      InvariantManager &IVM,
      llvm::ScalarEvolution &SE);

  LoopStructure *loop;
  LoopSCC *scc;
  llvm::PHINode *loopEntryPHI;
  std::unordered_set<llvm::PHINode *> stepPHIs;
  std::unordered_set<llvm::PHINode *> phis;
  std::unordered_set<llvm::Instruction *> nonPHIInstructions;
  llvm::Value *startValue;
  const llvm::SCEV *stepSCEV;
  llvm::Value *singleStepValue;
  int64_t stepMultiplier;
  std::vector<llvm::Instruction *> computationOfStepValue;
  bool isComputedStepValueLoopInvariant;
  llvm::Type *loopEntryPHIType;
  std::set<llvm::Value *> valuesToReferenceInComputingStepValue;
  std::set<llvm::Value *> valuesInScopeOfInductionVariable;
  std::unordered_set<llvm::Instruction *> instructions;
  std::unordered_set<llvm::Instruction *> derivedInstructions;
  std::unique_ptr<llvm::BasicBlock> stepComputationScratchBlock;
};

} // namespace loop
} // namespace analysis
} // namespace lotus
