/** @file LoopContent.h @brief Per-loop container owning incrementally materialized loop analyses. */
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

#include "Analysis/Loop/LoopForest.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
class Instruction;
class LoopInfo;
class ScalarEvolution;
class Value;
} // namespace llvm

namespace noelle {
class DominatorSummary;
} // namespace noelle

namespace pdg {
class ProgramGraph;
} // namespace pdg

namespace lotus {
namespace analysis {
namespace loop {

class InductionVariableManager;
class InvariantManager;
class LoopDependenceGraph;
class LoopEnvironment;
class LoopIterationSpaceAnalysis;
class LoopSCCDAG;
class MemoryCloningAnalysis;
class SCCDAGAttrs;
struct LoopDependenceBuilderOptions;

class LoopContent {
public:
  explicit LoopContent(LoopTree *loopNode);
  ~LoopContent();

  LoopContent(LoopContent &&) noexcept = default;
  LoopContent &operator=(LoopContent &&) noexcept = default;
  LoopContent(const LoopContent &) = delete;
  LoopContent &operator=(const LoopContent &) = delete;

  LoopTree *getLoopHierarchyStructures(void) const { return this->loop; }

  LoopStructure *getLoopStructure(void) const {
    return this->loop ? this->loop->getLoop() : nullptr;
  }

  LoopStructure *getNestedMostLoopStructure(llvm::Instruction *inst) const {
    return this->loop ? this->loop->getInnermostLoopThatContains(inst)
                      : nullptr;
  }

  bool hasDependenceGraph(void) const {
    return this->dependenceGraph != nullptr;
  }

  LoopDependenceGraph *getLoopDependenceGraph(void) const {
    assert(this->dependenceGraph != nullptr);
    return this->dependenceGraph.get();
  }

  bool hasSCCDAG(void) const { return this->sccdag != nullptr; }

  LoopSCCDAG *getSCCDAG(void) const {
    assert(this->sccdag != nullptr);
    return this->sccdag.get();
  }

  void materializeDependenceGraph(pdg::ProgramGraph &pdg);
  const std::vector<std::pair<std::string, std::string>> &
  getDependenceGraphDebugDumps(void) const {
    return this->dependenceGraphDebugDumps;
  }

  bool hasInvariantManager(void) const {
    return this->invariantManager != nullptr;
  }

  InvariantManager *getInvariantManager(void) const {
    assert(this->invariantManager != nullptr);
    return this->invariantManager.get();
  }

  bool hasInductionVariableManager(void) const {
    return this->inductionVariables != nullptr;
  }

  InductionVariableManager *getInductionVariableManager(void) const {
    assert(this->inductionVariables != nullptr);
    return this->inductionVariables.get();
  }

  void materializeScalarAnalyses(llvm::ScalarEvolution &SE,
                                 llvm::LoopInfo &LI,
                                 const noelle::DominatorSummary &DS);
  void materializeScalarAnalyses(llvm::ScalarEvolution &SE,
                                 llvm::LoopInfo &LI,
                                 const noelle::DominatorSummary &DS,
                                 const LoopDependenceBuilderOptions &options);

  bool hasEnvironment(void) const { return this->environment != nullptr; }

  LoopEnvironment *getEnvironment(void) const {
    assert(this->environment != nullptr);
    return this->environment.get();
  }

  void materializeEnvironment(void);

  void materializeLoopCarriedDependencies(const noelle::DominatorSummary &DS);

  bool hasLoopIterationSpaceAnalysis(void) const {
    return this->iterationSpaceAnalysis != nullptr;
  }

  LoopIterationSpaceAnalysis *getLoopIterationSpaceAnalysis(void) const {
    assert(this->iterationSpaceAnalysis != nullptr);
    return this->iterationSpaceAnalysis.get();
  }

  void materializeIterationSpaceAnalysis(llvm::ScalarEvolution &SE);

  bool hasMemoryCloningAnalysis(void) const {
    return this->memoryCloningAnalysis != nullptr;
  }

  MemoryCloningAnalysis *getMemoryCloningAnalysis(void) const {
    assert(this->memoryCloningAnalysis != nullptr);
    return this->memoryCloningAnalysis.get();
  }

  void materializeMemoryCloningAnalysis(const noelle::DominatorSummary &DS);

  bool hasSCCAttrs(void) const { return this->sccAttrs != nullptr; }

  SCCDAGAttrs *getSCCAttrs(void) const {
    assert(this->sccAttrs != nullptr);
    return this->sccAttrs.get();
  }

  void materializeSCCAttrs(bool enableFloatAsReal,
                           const noelle::DominatorSummary &DS);

  bool doesHaveCompileTimeKnownTripCount(void) const {
    return this->compileTimeKnownTripCount;
  }

  uint64_t getCompileTimeTripCount(void) const { return this->tripCount; }

  void setCompileTimeTripCount(uint64_t tripCount) {
    this->compileTimeKnownTripCount = (tripCount > 0);
    this->tripCount = tripCount;
  }

private:
  void invalidateAnalysesAfterDependenceGraph(void);
  void invalidateAnalysesAfterScalarAnalyses(void);
  void buildEnvironment(const std::set<llvm::Value *> &excludeValues);
  void materializeEnvironmentFromMemoryCloning(void);
  void materializeInductionVariables(
      llvm::ScalarEvolution &SE,
      llvm::LoopInfo &LI,
      const LoopDependenceBuilderOptions &options);

  LoopTree *loop;
  std::unique_ptr<LoopDependenceGraph> dependenceGraph;
  std::unique_ptr<LoopSCCDAG> sccdag;
  std::vector<std::pair<std::string, std::string>> dependenceGraphDebugDumps;
  std::unique_ptr<LoopDependenceGraph> ivDependenceGraph;
  std::unique_ptr<LoopSCCDAG> ivSCCDAG;
  std::unique_ptr<LoopEnvironment> environment;
  std::unique_ptr<InvariantManager> invariantManager;
  std::unique_ptr<InductionVariableManager> inductionVariables;
  std::unique_ptr<LoopIterationSpaceAnalysis> iterationSpaceAnalysis;
  std::unique_ptr<MemoryCloningAnalysis> memoryCloningAnalysis;
  std::unique_ptr<SCCDAGAttrs> sccAttrs;
  bool compileTimeKnownTripCount{false};
  uint64_t tripCount{0};
};

} // namespace loop
} // namespace analysis
} // namespace lotus
