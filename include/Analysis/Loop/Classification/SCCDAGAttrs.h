/** @file SCCDAGAttrs.h @brief Semantic classification driver for SCC DAG nodes in loop analysis. */
/*
 * Copyright 2026 Lotus contributors
 */
#pragma once

#include "Analysis/Loop/Classification/LoopCarriedVariable.h"
#include "Analysis/Loop/Classification/LoopSCCClassification.h"
#include "Analysis/Loop/LoopEnvironment.h"
#include "Analysis/Loop/Memory/LoopIterationSpaceAnalysis.h"
#include "Analysis/Loop/Memory/MemoryCloningAnalysis.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace llvm {
class Instruction;
class PHINode;
class Value;
class raw_ostream;
} // namespace llvm

namespace noelle {
class DominatorSummary;
} // namespace noelle

namespace lotus {
namespace analysis {
namespace loop {

class SCCDAGAttrs {
public:
  using SCCParentMap =
      std::unordered_map<LoopSCC *, std::unordered_set<LoopSCC *>>;
  using SCCEdgeSet = std::set<LoopSCCDAGEdge *>;
  using SCCEdgeMap = std::unordered_map<LoopSCC *, SCCEdgeSet>;

  SCCDAGAttrs(bool enableFloatAsReal,
              LoopDependenceGraph *loopDG,
              LoopSCCDAG *loopSCCDAG,
              LoopTree *loopNode,
              InductionVariableManager &IV,
              const noelle::DominatorSummary &DS,
              MemoryCloningAnalysis *sharedMemoryCloning = nullptr);

  GenericSCC *getSCCAttrs(LoopSCC *scc) const;
  std::set<LoopCarriedSCC *> getSCCsWithLoopCarriedDependencies(void) const;
  std::set<LoopCarriedSCC *> getSCCsWithLoopCarriedDataDependencies(void) const;
  std::set<LoopCarriedSCC *>
  getSCCsWithLoopCarriedControlDependencies(void) const;
  std::unordered_set<GenericSCC *> getSCCsOfKind(GenericSCC::SCCKind kind) const;
  bool isLoopGovernedBySCC(LoopSCC *scc) const;
  std::set<uint32_t> getLiveOutVariablesThatAreNotReducable(
      LoopEnvironment *env) const;
  bool isSCCContainedInSubloop(LoopTree *loopNode, LoopSCC *scc) const;
  std::pair<SCCParentMap, SCCEdgeMap> computeSCCDAGWhenSCCsAreIgnored(
      std::function<bool(GenericSCC *)> ignoreSCC) const;
  LoopSCCDAG *getSCCDAG(void) const;
  void print(llvm::raw_ostream &stream) const;

private:
  std::map<LoopSCC *, std::set<LoopDependenceEdge *>>
      sccToLoopCarriedDependencies;
  bool enableFloatAsReal;
  std::unordered_map<LoopSCC *, std::unique_ptr<GenericSCC>> sccToInfo;
  LoopDependenceGraph *loopDG;
  LoopSCCDAG *sccdag;
  std::unique_ptr<MemoryCloningAnalysis> ownedMemoryCloningAnalysis;
  MemoryCloningAnalysis *memoryCloningAnalysis{nullptr};

  void collectLoopCarriedDependencies(LoopTree *loopNode);
  std::unique_ptr<LoopCarriedVariable> checkIfReducible(LoopSCC *scc,
                                                        LoopTree *loopNode);
  std::tuple<bool, llvm::Value *, llvm::Value *, llvm::Value *, llvm::PHINode *>
  checkIfPeriodic(LoopSCC *scc, LoopTree *loopNode);
  bool checkIfIndependent(LoopSCC *scc);
  std::set<InductionVariable *> checkIfSCCOnlyContainsInductionVariables(
      LoopSCC *scc,
      LoopTree *loopNode,
      const std::set<InductionVariable *> &IVs,
      const std::set<InductionVariable *> &loopGoverningIVs) const;
  std::set<llvm::Instruction *> checkIfRecomputable(LoopSCC *scc,
                                                    LoopTree *loopNode) const;
  std::set<ClonableMemoryObject *> checkIfClonableByUsingLocalMemory(
      LoopSCC *scc,
      LoopTree *loopNode) const;
};

} // namespace loop
} // namespace analysis
} // namespace lotus
