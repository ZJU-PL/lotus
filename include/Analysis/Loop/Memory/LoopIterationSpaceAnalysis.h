/** @file LoopIterationSpaceAnalysis.h @brief Analysis of loop iteration spaces and cross-iteration disjointness. */
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

#include "Analysis/Loop/Dependence/LoopDependenceGraph.h"
#include "Analysis/Loop/Scalar/InductionVariables.h"

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace llvm {
class Instruction;
class SCEV;
class SCEVAddRecExpr;
class ScalarEvolution;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace loop {

class LoopIterationSpaceAnalysis {
public:
  LoopIterationSpaceAnalysis(LoopTree *loops,
                             InductionVariableManager &ivManager,
                             llvm::ScalarEvolution &SE);

  bool areInstructionsAccessingDisjointMemoryLocationsBetweenIterations(
      llvm::Instruction *from,
      llvm::Instruction *to) const;

private:
  struct MemoryAccessSpace {
    explicit MemoryAccessSpace(llvm::Instruction *memoryAccessor);

    llvm::Instruction *memoryAccessor;
    const llvm::SCEV *memoryAccessorSCEV{nullptr};
    const llvm::SCEV *memoryAccessorBasePointerSCEV{nullptr};
    const llvm::SCEV *memoryMinusSCEV{nullptr};
    const llvm::SCEVAddRecExpr *recurrence{nullptr};
    const llvm::SCEV *elementSize{nullptr};
    std::vector<const llvm::SCEV *> subscripts;
    std::vector<const llvm::SCEV *> sizes;
    std::vector<std::pair<llvm::Instruction *, InductionVariable *>> subscriptIVs;
    std::set<llvm::Instruction *> accessInstructions;
    uint64_t maxAccessSizeBytes{0};
    int64_t constantStep{0};
    bool isAnalyzed{false};
  };

  LoopTree *loops;
  InductionVariableManager &ivManager;
  std::unordered_map<const llvm::SCEV *,
                     std::unordered_set<llvm::Instruction *>>
      ivInstructionsBySCEV;
  std::unordered_map<const llvm::SCEV *,
                     std::unordered_set<llvm::Instruction *>>
      derivedInstructionsFromIVsBySCEV;
  std::unordered_map<llvm::Instruction *, InductionVariable *> ivsByInstruction;
  std::set<MemoryAccessSpace *> nonOverlappingAccessesBetweenIterations;
  std::unordered_map<MemoryAccessSpace *, std::set<MemoryAccessSpace *>>
      spacesThatCannotOverlap;
  std::vector<std::unique_ptr<MemoryAccessSpace>> accessSpaces;
  std::unordered_map<llvm::Instruction *, MemoryAccessSpace *>
      accessSpaceByInstruction;

  void indexIVInstructionSCEVs(llvm::ScalarEvolution &SE);
  void computeMemoryAccessSpace(llvm::ScalarEvolution &SE);
  void identifyIVForMemoryAccessSubscripts(llvm::ScalarEvolution &SE);
  void identifyNonOverlappingAccessesBetweenIterationsAcrossOneLoopInvocation(
      llvm::ScalarEvolution &SE);
  bool areMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
      MemoryAccessSpace *space1,
      MemoryAccessSpace *space2) const;
  bool analyzeToCheckIfMemoryAccessSpaceNotOverlappingOrExactlyTheSame(
      MemoryAccessSpace *space1,
      MemoryAccessSpace *space2) const;
  bool isMemoryAccessSpaceEquivalentForTopLoopIVSubscript(
      MemoryAccessSpace *space1,
      MemoryAccessSpace *space2) const;
  bool isOneToOneFunctionOnIV(llvm::ScalarEvolution &SE,
                              LoopStructure *loopStructure,
                              InductionVariable *IV,
                              llvm::Instruction *derivedInstruction) const;
  bool isInnerDimensionSubscriptsBounded(llvm::ScalarEvolution &SE,
                                         MemoryAccessSpace *space);
};

} // namespace loop
} // namespace analysis
} // namespace lotus
