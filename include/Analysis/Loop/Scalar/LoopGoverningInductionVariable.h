/** @file LoopGoverningInductionVariable.h @brief Analysis of induction variables that govern loop trip counts and exits. */
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

#include "Analysis/Loop/Scalar/InductionVariable.h"

#include <set>
#include <vector>

namespace llvm {
class BasicBlock;
class BranchInst;
class CmpInst;
class Instruction;
class Value;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace loop {

class LoopGoverningInductionVariable {
public:
  LoopGoverningInductionVariable(LoopStructure *loop, InductionVariable &iv);
  LoopGoverningInductionVariable(
      LoopStructure *loop,
      InductionVariable &iv,
      LoopSCC &scc,
      const std::vector<llvm::BasicBlock *> &exitBlocks);
  LoopGoverningInductionVariable(
      LoopStructure *loop,
      InductionVariable &iv,
      const std::vector<llvm::BasicBlock *> &exitBlocks);

  InductionVariable *getInductionVariable(void) const;
  llvm::CmpInst *getHeaderCompareInstructionToComputeExitCondition(void) const;
  llvm::Value *getExitConditionValue(void) const;
  llvm::BranchInst *getHeaderBrInst(void) const;
  llvm::BasicBlock *getExitBlockFromHeader(void) const;
  bool valueOfExitConditionToJumpToTheLoopBody(void) const;
  bool isSCCContainingIVWellFormed(void) const;
  std::set<llvm::Instruction *> getConditionValueDerivation(void) const;
  llvm::Instruction *getValueToCompareAgainstExitConditionValue(void) const;

private:
  LoopStructure *loop;
  InductionVariable *iv;
  std::set<llvm::Instruction *> conditionValueDerivation;
  llvm::CmpInst *headerCmp;
  llvm::BranchInst *headerBr;
  llvm::Value *conditionValue;
  llvm::Instruction *comparedValue;
  llvm::BasicBlock *exitBlock;
  bool isWellFormed;
};

} // namespace loop
} // namespace analysis
} // namespace lotus
