/**
 * @file SparseBitVectorGraph.h
 * @brief Dense-indexed sparse-bit-vector graph for Andersen's offline passes.
 */

#pragma once

#include "Alias/InclusionBased/SparrowAA/GraphTraits.h"
#include "Alias/InclusionBased/SparrowAA/NodeFactory.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <llvm/ADT/SparseBitVector.h>

class SparseBitVectorGraphNode {
private:
  NodeIndex idx = 0;
  llvm::SparseBitVector<512> succs;

  void insertEdge(NodeIndex n) { succs.set(n); }

public:
  using iterator = llvm::SparseBitVector<512>::iterator;

  SparseBitVectorGraphNode() = default;
  explicit SparseBitVectorGraphNode(NodeIndex i) : idx(i) {}

  NodeIndex getNodeIndex() const { return idx; }
  iterator begin() const { return succs.begin(); }
  iterator end() const { return succs.end(); }
  bool empty() const { return succs.empty(); }
  unsigned succ_getSize() const { return succs.count(); }

  friend class SparseBitVectorGraph;
};

// Fixed/dense-indexed graph backed by std::vector<SparseBitVectorGraphNode>.
// Avoids std::unordered_map bucket allocation, hashing, and rehash invalidation
// during HVN, HU, and HCD passes.
class SparseBitVectorGraph {
private:
  std::vector<SparseBitVectorGraphNode> nodes;
  std::vector<uint8_t> present;
  std::vector<NodeIndex> activeNodes;

  void ensureSize(NodeIndex idx) {
    if (idx < nodes.size())
      return;
    NodeIndex oldSize = static_cast<NodeIndex>(nodes.size());
    NodeIndex newSize = std::max(idx + 1, oldSize * 2);
    nodes.reserve(newSize);
    for (NodeIndex i = oldSize; i < newSize; ++i)
      nodes.emplace_back(i);
    present.resize(newSize, 0);
  }

  SparseBitVectorGraphNode *markPresent(NodeIndex idx) {
    ensureSize(idx);
    if (!present[idx]) {
      present[idx] = 1;
      activeNodes.push_back(idx);
    }
    return &nodes[idx];
  }

public:
  using iterator = std::vector<SparseBitVectorGraphNode>::iterator;
  using const_iterator = std::vector<SparseBitVectorGraphNode>::const_iterator;

  SparseBitVectorGraph() = default;

  explicit SparseBitVectorGraph(unsigned numNodes) : present(numNodes, 0) {
    nodes.reserve(numNodes);
    for (unsigned i = 0; i < numNodes; ++i)
      nodes.emplace_back(i);
  }

  SparseBitVectorGraphNode *getOrInsertNode(NodeIndex idx) {
    return markPresent(idx);
  }

  void insertEdge(NodeIndex src, NodeIndex dst) {
    ensureSize(std::max(src, dst));
    markPresent(src)->insertEdge(dst);
    markPresent(dst);
  }

  // src's successors += dst's successors
  void mergeEdge(NodeIndex src, NodeIndex dst) {
    if (dst >= nodes.size() || !present[dst])
      return;
    ensureSize(src);
    markPresent(src)->succs |= nodes[dst].succs;
  }

  SparseBitVectorGraphNode *getNodeWithIndex(NodeIndex idx) {
    if (idx >= nodes.size() || !present[idx])
      return nullptr;
    return &nodes[idx];
  }

  const SparseBitVectorGraphNode *getNodeWithIndex(NodeIndex idx) const {
    if (idx >= nodes.size() || !present[idx])
      return nullptr;
    return &nodes[idx];
  }

  const std::vector<NodeIndex> &getActiveNodes() const { return activeNodes; }

  unsigned getSize() const { return static_cast<unsigned>(activeNodes.size()); }

  void releaseMemory() {
    nodes.clear();
    present.clear();
    activeNodes.clear();
  }

  iterator begin() { return nodes.begin(); }
  iterator end() { return nodes.end(); }
  const_iterator begin() const { return nodes.begin(); }
  const_iterator end() const { return nodes.end(); }
};

// Specialize AndersGraphTraits for SparseBitVectorGraph
template <> class AndersGraphTraits<SparseBitVectorGraph> {
public:
  using NodeType = SparseBitVectorGraphNode;
  using NodeIterator = SparseBitVectorGraph::iterator;
  using ChildIterator = SparseBitVectorGraphNode::iterator;

  static inline ChildIterator child_begin(NodeType *n) { return n->begin(); }
  static inline ChildIterator child_end(NodeType *n) { return n->end(); }

  static inline NodeIterator node_begin(SparseBitVectorGraph *g) {
    return g->begin();
  }
  static inline NodeIterator node_end(SparseBitVectorGraph *g) {
    return g->end();
  }
};
