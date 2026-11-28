/** @file LoopSCCDAG.h @brief Strongly-connected-component DAG condensation over a loop dependence graph. */
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

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus {
namespace analysis {
namespace loop {

class LoopSCC;

class LoopSCCDAGEdge {
public:
  LoopSCC *getSrc(void) const;
  LoopSCC *getDst(void) const;
  std::vector<LoopDependenceEdge *> getSubEdges(void) const;
  uint64_t getNumberOfSubEdges(void) const;
  bool isLoopCarried(void) const;

private:
  friend class LoopSCCDAG;

  LoopSCCDAGEdge(LoopSCC *src, LoopSCC *dst);
  void addSubEdge(LoopDependenceEdge *edge);

  LoopSCC *src;
  LoopSCC *dst;
  std::vector<LoopDependenceEdge *> subEdges;
};

class LoopSCC {
public:
  uint64_t getID(void) const;
  bool hasCycle(void) const;
  bool isIncludedInLoop(void) const;

  std::vector<LoopDependenceNode *> getNodes(void) const;
  std::vector<LoopSCC *> getPredecessors(void) const;
  std::vector<LoopSCC *> getSuccessors(void) const;
  std::vector<LoopSCCDAGEdge *> getIncomingEdges(void) const;
  std::vector<LoopSCCDAGEdge *> getOutgoingEdges(void) const;
  std::vector<LoopDependenceEdge *> getEdges(void) const;

  std::vector<std::pair<llvm::Value *, LoopDependenceNode *>>
  internalNodePairs(void) const;
  std::vector<std::pair<llvm::Value *, LoopDependenceNode *>>
  externalNodePairs(void) const;
  bool isInternal(llvm::Value *value) const;
  bool isExternal(llvm::Value *value) const;
  LoopDependenceNode *fetchNode(llvm::Value *value) const;

  int64_t numberOfInstructions(void) const;
  bool iterateOverInstructions(
      const std::function<bool(llvm::Instruction *)> &funcToInvoke) const;
  bool iterateOverAllInstructions(
      const std::function<bool(llvm::Instruction *)> &funcToInvoke) const;
  bool iterateOverValues(
      const std::function<bool(llvm::Value *)> &funcToInvoke) const;
  bool iterateOverAllValues(
      const std::function<bool(llvm::Value *)> &funcToInvoke) const;

private:
  friend class LoopSCCDAG;

  LoopSCC(uint64_t id,
          std::vector<LoopDependenceNode *> members,
          bool hasCycle,
          bool includedInLoop);

  uint64_t id;
  std::vector<LoopDependenceNode *> internalNodes;
  std::vector<LoopDependenceNode *> externalNodes;
  std::vector<LoopSCC *> predecessors;
  std::vector<LoopSCC *> successors;
  std::vector<LoopSCCDAGEdge *> incomingEdges;
  std::vector<LoopSCCDAGEdge *> outgoingEdges;
  bool cycle;
  bool includedInLoop;
};

class LoopSCCDAG {
public:
  using SCCSet = std::vector<LoopSCC *>;

  explicit LoopSCCDAG(LoopDependenceGraph &graph);

  LoopDependenceGraph *getLoopDependenceGraph(void) const;

  std::vector<LoopSCC *> getSCCs(void) const;
  std::vector<LoopSCC *> getInternalSCCs(void) const;
  std::vector<LoopSCC *> getExternalSCCs(void) const;
  std::vector<LoopSCC *> getAllSCCs(void) const;
  std::vector<LoopSCCDAGEdge *> getEdges(void) const;
  LoopSCC *getSCC(llvm::Value *value) const;
  bool doesItContain(llvm::Instruction *inst) const;
  bool iterateOverLiveInAndLiveOut(
      const std::function<bool(llvm::Value *)> &funcToInvoke) const;
  bool orderedBefore(const LoopSCC *early, const LoopSCC *late) const;
  bool orderedBefore(const LoopSCC *early, const SCCSet &lates) const;
  bool orderedBefore(const SCCSet &earlies, const LoopSCC *late) const;
  uint32_t getSCCIndex(const LoopSCC *scc) const;

private:
  LoopDependenceGraph *graph;
  std::vector<LoopDependenceNode *> internalNodes;
  std::vector<LoopDependenceNode *> allNodes;
  std::vector<std::unique_ptr<LoopSCC>> ownedSCCs;
  std::vector<std::unique_ptr<LoopSCCDAGEdge>> ownedEdges;
  std::vector<LoopSCC *> includedSCCs;
  std::vector<LoopSCC *> allSCCs;
  std::map<std::pair<LoopSCC *, LoopSCC *>, LoopSCCDAGEdge *> edgeBySCCPair;
  std::unordered_map<LoopDependenceNode *, LoopSCC *> sccByNode;
  std::unordered_map<llvm::Value *, LoopSCC *> sccByValue;
  std::unordered_map<const LoopSCC *, uint32_t> sccIndexes;
  std::unordered_map<const LoopSCC *, std::unordered_set<const LoopSCC *>>
      reachableSCCs;

  void computeReachabilityAmongSCCs(void);
};

} // namespace loop
} // namespace analysis
} // namespace lotus
