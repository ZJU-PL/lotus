/** @file LoopDependenceGraph.h @brief Loop dependence graph representation for intra-loop dependency analysis. */
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
#include "IR/PDG/Core/PDGEnums.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pdg {
class Edge;
class Node;
class ProgramGraph;
} // namespace pdg

namespace lotus {
namespace analysis {
namespace loop {

enum class LoopDependenceEdgeKind { Variable, Control, Memory };

enum class LoopDependenceMemoryKind {
  None,
  Raw,
  ReadOnly,
  AliasUnknown,
  Unknown
};

enum class LoopDependenceEdgeOrigin {
  ImportedPDG,
  SynthesizedBoundaryValue,
  SynthesizedRefinement
};

class LoopDependenceNode;
class LoopDependenceEdge;
class LoopDependenceBuilder;

class LoopDependenceNode {
public:
  uint64_t getID(void) const;
  llvm::Value *getValue(void) const;
  pdg::Node *getPDGNode(void) const;
  bool isInternal(void) const;
  bool isExternal(void) const;

  const std::vector<LoopDependenceEdge *> &getIncomingEdges(void) const;
  const std::vector<LoopDependenceEdge *> &getOutgoingEdges(void) const;

  bool hasIncomingEdges(void) const;
  bool hasOutgoingEdges(void) const;

private:
  friend class LoopDependenceGraph;

  LoopDependenceNode(uint64_t id, llvm::Value *value, pdg::Node *pdgNode,
                     bool internal);

  uint64_t id;
  llvm::Value *value;
  pdg::Node *pdgNode;
  bool internal;
  std::vector<LoopDependenceEdge *> incomingEdges;
  std::vector<LoopDependenceEdge *> outgoingEdges;
};

class LoopDependenceEdge {
public:
  LoopDependenceNode *getSrc(void) const;
  LoopDependenceNode *getDst(void) const;
  LoopDependenceEdgeKind getKind(void) const;
  LoopDependenceMemoryKind getMemoryKind(void) const;
  LoopDependenceEdgeOrigin getOrigin(void) const;
  pdg::EdgeType getOriginalEdgeType(void) const;
  bool isLoopCarried(void) const;
  void setLoopCarried(bool isLoopCarried);

private:
  friend class LoopDependenceGraph;

  LoopDependenceEdge(LoopDependenceNode *src, LoopDependenceNode *dst,
                     LoopDependenceEdgeKind kind,
                     LoopDependenceMemoryKind memoryKind,
                     LoopDependenceEdgeOrigin origin,
                     pdg::EdgeType originalEdgeType, bool loopCarried);

  LoopDependenceNode *src;
  LoopDependenceNode *dst;
  LoopDependenceEdgeKind kind;
  LoopDependenceMemoryKind memoryKind;
  LoopDependenceEdgeOrigin origin;
  pdg::EdgeType originalEdgeType;
  bool loopCarried;
};

class LoopDependenceGraph {
public:
  LoopDependenceGraph(LoopTree *loopNode, pdg::ProgramGraph &pdg);

  LoopTree *getLoopHierarchyStructures(void) const;
  LoopStructure *getLoopStructure(void) const;

  std::vector<LoopDependenceNode *> getInternalNodes(void) const;
  std::vector<LoopDependenceNode *> getExternalNodes(void) const;
  std::vector<LoopDependenceNode *> getNodes(void) const;
  std::vector<LoopDependenceEdge *> getEdges(void) const;
  std::vector<LoopDependenceEdge *> getDependences(llvm::Value *src,
                                                   llvm::Value *dst) const;
  std::vector<LoopDependenceEdge *> getSortedDependences(void) const;

  std::vector<std::pair<llvm::Value *, LoopDependenceNode *>>
  internalNodePairs(void) const;
  std::vector<std::pair<llvm::Value *, LoopDependenceNode *>>
  externalNodePairs(void) const;

  LoopDependenceNode *getNode(llvm::Value *value) const;
  bool doesItContain(llvm::Value *value) const;
  bool isInternal(llvm::Value *value) const;
  bool isExternal(llvm::Value *value) const;

  bool iterateOverDependencesTo(
      llvm::Value *target, bool includeControl, bool includeVariable,
      bool includeMemory,
      const std::function<bool(llvm::Value *, LoopDependenceEdge *)>
          &funcToInvoke) const;
  bool iterateOverDependencesFrom(
      llvm::Value *source, bool includeControl, bool includeVariable,
      bool includeMemory,
      const std::function<bool(llvm::Value *, LoopDependenceEdge *)>
          &funcToInvoke) const;

  std::unique_ptr<LoopDependenceGraph> createSubgraph(bool includeControl,
                                                      bool includeVariable,
                                                      bool includeMemory) const;
  std::unique_ptr<LoopDependenceGraph> createSubgraphFromValues(
      const std::vector<llvm::Value *> &values,
      bool linkToExternal,
      bool includeControl = true,
      bool includeVariable = true,
      bool includeMemory = true,
      const std::unordered_set<LoopDependenceEdge *> &edgesToIgnore = {}) const;

  void removeEdge(LoopDependenceEdge *edge);
  void addVariableDependence(llvm::Value *src, llvm::Value *dst,
                             bool loopCarried = false);
  std::string renderStable(void) const;

  static std::string describeValue(const llvm::Value *value);
  static bool shouldIgnoreInstruction(const llvm::Instruction *instruction);
  static bool shouldIgnoreValue(const llvm::Value *value);
  static bool isAllowedBoundaryContextValue(const llvm::Value *value);

private:
  friend class LoopDependenceBuilder;
  LoopDependenceGraph() = default;

  LoopTree *loop{nullptr};
  pdg::ProgramGraph *pdg{nullptr};
  std::vector<std::unique_ptr<LoopDependenceNode>> ownedNodes;
  std::vector<LoopDependenceNode *> internalNodes;
  std::vector<LoopDependenceNode *> externalNodes;
  std::vector<std::unique_ptr<LoopDependenceEdge>> ownedEdges;
  std::unordered_map<llvm::Value *, LoopDependenceNode *> nodesByValue;
  std::unordered_map<pdg::Node *, LoopDependenceNode *> nodesByPDGNode;

  LoopDependenceNode *fetchOrCreateNode(llvm::Value *value, pdg::Node *pdgNode,
                                        bool internal);
  void importEdgeIfIncluded(pdg::Edge *edge,
                            bool linkToExternal,
                            const std::unordered_set<llvm::Value *>
                                &internalValues,
                            std::unordered_set<pdg::Edge *> &seenEdges);
  void initialize(LoopTree *loopNode, pdg::ProgramGraph &programGraph);
  void addEdge(LoopDependenceNode *src, LoopDependenceNode *dst,
               LoopDependenceEdgeKind kind,
               LoopDependenceMemoryKind memoryKind,
               LoopDependenceEdgeOrigin origin, pdg::EdgeType originalEdgeType,
               bool loopCarried);
};

} // namespace loop
} // namespace analysis
} // namespace lotus
