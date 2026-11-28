/** @file InductionVariables.h @brief Induction variable manager for a loop nest. */
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
#include "Analysis/Loop/Scalar/InductionVariable.h"
#include "Analysis/Loop/Scalar/LoopGoverningInductionVariable.h"
#include "Analysis/Loop/Scalar/LoopInvariants.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {
class Instruction;
class LoopInfo;
class ScalarEvolution;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace loop {

class InductionVariableManager {
public:
  InductionVariableManager(LoopTree *loop,
                           InvariantManager &invariants,
                           llvm::ScalarEvolution &SE,
                           llvm::LoopInfo &LI,
                           LoopSCCDAG &sccdag,
                           LoopEnvironment &loopEnvironment,
                           bool enableExtendedRecognition = false);

  std::unordered_set<InductionVariable *> getInductionVariables(void) const;
  std::unordered_set<InductionVariable *> getInductionVariables(
      llvm::Instruction *inst) const;
  std::unordered_set<InductionVariable *> getInductionVariables(
      LoopStructure &loop) const;
  InductionVariable *getInductionVariable(LoopStructure &loop,
                                          llvm::Instruction *inst) const;
  bool doesContributeToComputeAnInductionVariable(llvm::Instruction *inst) const;
  InductionVariable *getDerivingInductionVariable(
      LoopStructure &loop,
      llvm::Instruction *derivedInstruction) const;
  LoopGoverningInductionVariable *getLoopGoverningInductionVariable(void) const;
  LoopGoverningInductionVariable *getLoopGoverningInductionVariable(
      LoopStructure &loop) const;

private:
  LoopTree *loop;
  std::unordered_map<LoopStructure *,
                     std::vector<std::unique_ptr<InductionVariable>>>
      ownedIVs;
  std::unordered_map<LoopStructure *, LoopGoverningInductionVariable *>
      governingIVs;
  std::vector<std::unique_ptr<LoopGoverningInductionVariable>>
      ownedGoverningIVs;
};

} // namespace loop
} // namespace analysis
} // namespace lotus
