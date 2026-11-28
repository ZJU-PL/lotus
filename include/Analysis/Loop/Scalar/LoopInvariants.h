/** @file LoopInvariants.h @brief Loop invariant detection and classification. */
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
#include "Analysis/Loop/Dependence/LoopSCCDAG.h"

#include <unordered_set>

namespace lotus {
namespace analysis {
namespace loop {

class InvariantManager {
public:
  InvariantManager(LoopStructure *loop, LoopDependenceGraph *loopDG);

  bool isLoopInvariant(llvm::Value *value) const;
  bool isLoopInvariant(LoopSCC *scc) const;
  std::unordered_set<llvm::Instruction *>
  getLoopInstructionsThatAreLoopInvariants(void) const;

private:
  class InvarianceChecker;

  LoopStructure *loop;
  LoopDependenceGraph *loopDG;
  std::unordered_set<llvm::Instruction *> invariants;
};

} // namespace loop
} // namespace analysis
} // namespace lotus
