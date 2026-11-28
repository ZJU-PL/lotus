/** @file LoopCarriedDependencies.h @brief Analysis of loop-carried dependence edges. */
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

#include <set>

namespace noelle {
class DominatorSummary;
} // namespace noelle

namespace lotus {
namespace analysis {
namespace loop {

class LoopCarriedDependencies {
public:
  static void setLoopCarriedDependencies(LoopTree *loopNode,
                                         const noelle::DominatorSummary &DS,
                                         LoopDependenceGraph &loopDG);

  static std::set<LoopDependenceEdge *> getLoopCarriedDependenciesForLoop(
      const LoopStructure &loop,
      LoopTree *loopNode,
      LoopDependenceGraph &loopDG);

  static std::set<LoopDependenceEdge *> getLoopCarriedDependenciesForLoop(
      const LoopStructure &loop,
      LoopTree *loopNode,
      LoopSCCDAG &sccdag);

private:
  static bool isALoopCarriedDependence(LoopTree *loopNode,
                                       const noelle::DominatorSummary &DS,
                                       LoopDependenceEdge *edge);

  static bool canBasicBlockReachHeaderBeforeOther(const LoopStructure &loop,
                                                  llvm::BasicBlock *from,
                                                  llvm::BasicBlock *other);
};

} // namespace loop
} // namespace analysis
} // namespace lotus
