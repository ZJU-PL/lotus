/**
 * @file ConstraintSolving.cpp
 * @brief Constraint solving phase using worklist-based delta propagation.
 *
 * This file implements the constraint solving algorithm that propagates
 * points-to information through the constraint graph. Uses dense node arrays,
 * difference (delta) propagation, and optional cycle detection (HCD: Hybrid
 * Cycle Detection, LCD: Lazy Cycle Detection).
 *
 * @author rainoftime
 */
#include "Alias/InclusionBased/SparrowAA/Andersen.h"
#include "Alias/InclusionBased/SparrowAA/CycleDetector.h"
#include "Alias/InclusionBased/SparrowAA/Log.h"
#include "Alias/InclusionBased/SparrowAA/SparseBitVectorGraph.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SparseBitVector.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "andersen"

using namespace llvm;

STATISTIC(NumIterations, "Number of constraint solving iterations");
STATISTIC(NumWorkListProcessed, "Number of work list items processed");
STATISTIC(NumCopyEdgesAdded, "Number of copy edges added");
STATISTIC(NumLoadEdgesProcessed, "Number of load edges processed");
STATISTIC(NumStoreEdgesProcessed, "Number of store edges processed");
STATISTIC(NumCyclesDetected, "Number of cycles detected online");
STATISTIC(NumNodesCollapsed, "Number of nodes collapsed during solving");
STATISTIC(NumPtsSetUnions, "Number of points-to set unions");
STATISTIC(MaxPtsSetSize, "Maximum points-to set size");
STATISTIC(AvgPtsSetSize, "Average points-to set size");

// Use extern to reference the category defined in Andersen.cpp
extern cl::OptionCategory AndersenCategory;

cl::opt<bool> EnableHCD("enable-hcd",
                        cl::desc("Enable the hybrid cycle detection algorithm"),
                        cl::init(true), cl::cat(AndersenCategory));
cl::opt<bool> EnableLCD("enable-lcd",
                        cl::desc("Enable the lazy cycle detection algorithm"),
                        cl::init(true), cl::cat(AndersenCategory));

namespace {

// Dense constraint graph node backed by 512-bit SparseBitVectors for compact
// storage and fast bitwise unions when collapsing cycles.
class ConstraintGraphNode {
private:
  NodeIndex idx = 0;
  using EdgeSet = llvm::SparseBitVector<512>;
  EdgeSet copyEdges, loadEdges, storeEdges;

public:
  using iterator = EdgeSet::iterator;
  using const_iterator = EdgeSet::iterator;

  ConstraintGraphNode() = default;
  explicit ConstraintGraphNode(NodeIndex i) : idx(i) {}

  NodeIndex getNodeIndex() const { return idx; }

  bool insertCopyEdge(NodeIndex dst) { return copyEdges.test_and_set(dst); }
  bool mergeCopyEdges(const EdgeSet &other, NodeIndex self) {
    bool changed = (copyEdges |= other);
    if (changed)
      copyEdges.reset(self);
    return changed;
  }
  void removeCopyEdge(NodeIndex dst) { copyEdges.reset(dst); }
  bool insertLoadEdge(NodeIndex dst) { return loadEdges.test_and_set(dst); }
  void removeLoadEdge(NodeIndex dst) { loadEdges.reset(dst); }
  bool insertStoreEdge(NodeIndex dst) { return storeEdges.test_and_set(dst); }
  void removeStoreEdge(NodeIndex dst) { storeEdges.reset(dst); }

  const EdgeSet &getLoadEdges() const { return loadEdges; }
  const EdgeSet &getStoreEdges() const { return storeEdges; }

  bool hasEdges() const {
    return !copyEdges.empty() || !loadEdges.empty() || !storeEdges.empty();
  }
  bool hasLoads() const { return !loadEdges.empty(); }
  bool hasStores() const { return !storeEdges.empty(); }
  bool hasCopies() const { return !copyEdges.empty(); }

  void mergeEdges(const ConstraintGraphNode &other) {
    copyEdges |= other.copyEdges;
    loadEdges |= other.loadEdges;
    storeEdges |= other.storeEdges;
  }

  void clear() {
    copyEdges.clear();
    loadEdges.clear();
    storeEdges.clear();
  }

  void replaceCopyEdge(NodeIndex oldIdx, NodeIndex newIdx) {
    copyEdges.reset(oldIdx);
    copyEdges.set(newIdx);
  }
  void replaceLoadEdge(NodeIndex oldIdx, NodeIndex newIdx) {
    loadEdges.reset(oldIdx);
    loadEdges.set(newIdx);
  }
  void replaceStoreEdge(NodeIndex oldIdx, NodeIndex newIdx) {
    storeEdges.reset(oldIdx);
    storeEdges.set(newIdx);
  }

  iterator begin() const { return copyEdges.begin(); }
  iterator end() const { return copyEdges.end(); }

  iterator load_begin() const { return loadEdges.begin(); }
  iterator load_end() const { return loadEdges.end(); }
  llvm::iterator_range<iterator> loads() const {
    return llvm::iterator_range<iterator>(load_begin(), load_end());
  }

  iterator store_begin() const { return storeEdges.begin(); }
  iterator store_end() const { return storeEdges.end(); }
  llvm::iterator_range<iterator> stores() const {
    return llvm::iterator_range<iterator>(store_begin(), store_end());
  }
};

// Fixed-size dense constraint graph indexed directly by NodeIndex.
// Because `nodes` is sized once to `numNodes`, pointers to nodes never
// invalidate during solving or online cycle collapsing.
class ConstraintGraph {
private:
  std::vector<ConstraintGraphNode> nodes;

public:
  using iterator = std::vector<ConstraintGraphNode>::iterator;
  using const_iterator = std::vector<ConstraintGraphNode>::const_iterator;

  explicit ConstraintGraph(unsigned numNodes) {
    nodes.reserve(numNodes);
    for (unsigned i = 0; i < numNodes; ++i)
      nodes.emplace_back(i);
  }

  bool insertCopyEdge(NodeIndex src, NodeIndex dst) {
    if (src == dst)
      return false;
    return nodes[src].insertCopyEdge(dst);
  }

  bool insertLoadEdge(NodeIndex src, NodeIndex dst) {
    return nodes[src].insertLoadEdge(dst);
  }

  bool insertStoreEdge(NodeIndex src, NodeIndex dst) {
    return nodes[src].insertStoreEdge(dst);
  }

  bool mergeNodes(NodeIndex dst, NodeIndex src) {
    if (!nodes[src].hasEdges())
      return false;
    nodes[dst].mergeEdges(nodes[src]);
    return true;
  }

  void deleteNode(NodeIndex idx) { nodes[idx].clear(); }

  ConstraintGraphNode *getNodeWithIndex(NodeIndex idx) { return &nodes[idx]; }
  ConstraintGraphNode *getOrInsertNode(NodeIndex idx) { return &nodes[idx]; }

  iterator begin() { return nodes.begin(); }
  iterator end() { return nodes.end(); }
  const_iterator begin() const { return nodes.begin(); }
  const_iterator end() const { return nodes.end(); }
};

} // namespace

// Specialize AndersGraphTraits for ConstraintGraph
template <> class AndersGraphTraits<ConstraintGraph> {
public:
  using NodeType = ConstraintGraphNode;
  using NodeIterator = ConstraintGraph::const_iterator;
  using ChildIterator = ConstraintGraphNode::iterator;

  static inline ChildIterator child_begin(const NodeType *n) {
    return n->begin();
  }
  static inline ChildIterator child_end(const NodeType *n) { return n->end(); }

  static inline NodeIterator node_begin(const ConstraintGraph *g) {
    return g->begin();
  }
  static inline NodeIterator node_end(const ConstraintGraph *g) {
    return g->end();
  }
};

namespace {

// Dense O(1) FIFO worklist with byte membership bitset.
class AndersWorkList {
private:
  std::vector<NodeIndex> list;
  size_t head = 0;
  std::vector<uint8_t> inSet;

public:
  explicit AndersWorkList(unsigned numNodes) : inSet(numNodes, 0) {}

  void enqueue(NodeIndex elem) {
    if (!inSet[elem]) {
      inSet[elem] = 1;
      list.push_back(elem);
    }
  }

  NodeIndex dequeue() {
    assert(head < list.size() && "Trying to dequeue an empty queue!");
    NodeIndex ret = list[head++];
    inSet[ret] = 0;
    if (head == list.size()) {
      list.clear();
      head = 0;
    }
    return ret;
  }

  bool isEmpty() const { return head >= list.size(); }
};

template <typename PtsSetType>
static void collapseNodes(NodeIndex dst, NodeIndex src,
                          AndersNodeFactory &nodeFactory,
                          std::vector<PtsSetType> &ptsGraph,
                          std::vector<PtsSetType> &oldPtsGraph,
                          ConstraintGraph &constraintGraph,
                          AndersWorkList &nextWorkList) {
  if (dst == src)
    return;

  ++NumNodesCollapsed;

  nodeFactory.mergeNode(dst, src);
  bool ptsChanged = false;
  if (!ptsGraph[src].isEmpty()) {
    ptsChanged = ptsGraph[dst].unionWith(ptsGraph[src]);
    ptsGraph[src].clear();
  }

  bool edgesMerged = constraintGraph.mergeNodes(dst, src);
  bool oldShrunk = false;
  if (edgesMerged) {
    // Only elements already propagated along BOTH dst's and src's edges remain
    // propagated along the merged edge set.
    oldShrunk = oldPtsGraph[dst].intersectAssign(oldPtsGraph[src]);
  }
  oldPtsGraph[src].clear();
  constraintGraph.deleteNode(src);

  if ((ptsChanged || oldShrunk) && !ptsGraph[dst].isEmpty())
    nextWorkList.enqueue(dst);
}

// Offline portion of HCD (Hybrid Cycle Detection, PLDI 2007).
class OfflineCycleDetector : public CycleDetector<SparseBitVectorGraph> {
private:
  AndersNodeFactory &nodeFactory;
  SparseBitVectorGraph offlineGraph;
  DenseMap<NodeIndex, NodeIndex> collapseMap;
  DenseMap<NodeIndex, NodeIndex> mergeMap;
  llvm::SparseBitVector<512> scc;

  NodeIndex getRefNodeIndex(NodeIndex n) const {
    return n + nodeFactory.getNumNodes();
  }

  void buildOfflineConstraintGraph(
      const std::vector<AndersConstraint> &constraints) {
    for (auto const &c : constraints) {
      NodeIndex srcTgt = nodeFactory.getMergeTarget(c.getSrc());
      NodeIndex dstTgt = nodeFactory.getMergeTarget(c.getDest());
      switch (c.getType()) {
      case AndersConstraint::ADDR_OF:
        break;
      case AndersConstraint::LOAD:
        offlineGraph.insertEdge(getRefNodeIndex(srcTgt), dstTgt);
        break;
      case AndersConstraint::STORE:
        offlineGraph.insertEdge(srcTgt, getRefNodeIndex(dstTgt));
        break;
      case AndersConstraint::COPY:
        if (srcTgt != dstTgt)
          offlineGraph.insertEdge(srcTgt, dstTgt);
        break;
      }
    }
  }

  NodeType *getRep(NodeIndex idx) override {
    return offlineGraph.getOrInsertNode(idx);
  }

  void processNodeOnCycle(const NodeType *node,
                          const NodeType *repNode) override {
    (void)repNode;
    scc.set(node->getNodeIndex());
  }

  void processCycleRepNode(const NodeType *node) override {
    if (scc.empty())
      return;

    scc.set(node->getNodeIndex());

    NodeIndex repNode = AndersNodeFactory::InvalidIndex;
    for (auto idx : scc) {
      if (idx < nodeFactory.getNumNodes()) {
        repNode = idx;
        break;
      }
    }
    if (repNode == AndersNodeFactory::InvalidIndex) {
      scc.clear();
      return;
    }

    for (auto cycleNode : scc) {
      if (cycleNode == repNode)
        continue;
      if (cycleNode >= nodeFactory.getNumNodes())
        collapseMap[cycleNode - nodeFactory.getNumNodes()] = repNode;
      else
        mergeMap[cycleNode] = repNode;
    }

    scc.clear();
  }

public:
  OfflineCycleDetector(const std::vector<AndersConstraint> &cs,
                       AndersNodeFactory &n)
      : nodeFactory(n), offlineGraph(n.getNumNodes() * 2) {
    reserveSCCState(n.getNumNodes() * 2);
    buildOfflineConstraintGraph(cs);
  }

  void run() override {
    runOnNodeList(offlineGraph.getActiveNodes());

    for (auto const &mapping : mergeMap)
      nodeFactory.mergeNode(mapping.second, mapping.first);

    mergeMap.clear();
    offlineGraph.releaseMemory();
    releaseSCCMemory();
  }

  NodeIndex getCollapseTarget(NodeIndex n) const {
    auto itr = collapseMap.find(n);
    if (itr == collapseMap.end())
      return AndersNodeFactory::InvalidIndex;
    return itr->second;
  }
};

template <typename PtsSetType>
void buildConstraintGraph(ConstraintGraph &cGraph,
                          const std::vector<AndersConstraint> &constraints,
                          AndersNodeFactory &nodeFactory,
                          std::vector<PtsSetType> &ptsGraph) {
  for (auto const &c : constraints) {
    NodeIndex srcTgt = nodeFactory.getMergeTarget(c.getSrc());
    NodeIndex dstTgt = nodeFactory.getMergeTarget(c.getDest());
    switch (c.getType()) {
    case AndersConstraint::ADDR_OF:
      ptsGraph[dstTgt].insert(c.getSrc());
      break;
    case AndersConstraint::LOAD:
      cGraph.insertLoadEdge(srcTgt, dstTgt);
      break;
    case AndersConstraint::STORE:
      cGraph.insertStoreEdge(dstTgt, srcTgt);
      break;
    case AndersConstraint::COPY:
      cGraph.insertCopyEdge(srcTgt, dstTgt);
      break;
    }
  }
}

// Batched online cycle detector (LCD, PLDI 2007): shares DFS state across all
// candidates in a batch so each reachable node is explored at most once.
template <typename PtsSetType>
class OnlineCycleDetectorT : public CycleDetector<ConstraintGraph> {
private:
  AndersNodeFactory &nodeFactory;
  ConstraintGraph &constraintGraph;
  std::vector<PtsSetType> &ptsGraph;
  std::vector<PtsSetType> &oldPtsGraph;
  AndersWorkList *nextWorkList;
  const std::vector<std::pair<NodeIndex, NodeIndex>> &cycleCandidates;

  NodeType *getRep(NodeIndex idx) override {
    return constraintGraph.getOrInsertNode(nodeFactory.getMergeTarget(idx));
  }

  void processNodeOnCycle(const NodeType *node,
                          const NodeType *repNode) override {
    NodeIndex repIdx = nodeFactory.getMergeTarget(repNode->getNodeIndex());
    NodeIndex cycleIdx = nodeFactory.getMergeTarget(node->getNodeIndex());
    collapseNodes(repIdx, cycleIdx, nodeFactory, ptsGraph, oldPtsGraph,
                  constraintGraph, *nextWorkList);
  }

  void processCycleRepNode(const NodeType *node) override { (void)node; }

public:
  OnlineCycleDetectorT(AndersNodeFactory &n, ConstraintGraph &co,
                       std::vector<PtsSetType> &p,
                       std::vector<PtsSetType> &oldP, AndersWorkList &nextWL,
                       const std::vector<std::pair<NodeIndex, NodeIndex>> &cc)
      : nodeFactory(n), constraintGraph(co), ptsGraph(p), oldPtsGraph(oldP),
        nextWorkList(&nextWL), cycleCandidates(cc) {
    reserveSCCState(n.getNumNodes());
  }

  void setNextWorkList(AndersWorkList &nextWL) { nextWorkList = &nextWL; }

  void run() override {
    resetVisitedState();
    for (const auto &candidate : cycleCandidates)
      runOnNodeBatched(candidate.first);
  }
};

} // end of anonymous namespace

template <typename PtsSetType>
void Andersen::solveConstraintsImpl(std::vector<PtsSetType> &ptsGraph) {
  const unsigned numNodes = nodeFactory.getNumNodes();
  ptsGraph.clear();
  ptsGraph.resize(numNodes);

  // Only build and run OfflineCycleDetector when HCD is enabled.
  std::unique_ptr<OfflineCycleDetector> offlineInfo;
  if (EnableHCD) {
    LOG_INFO("Creating offline cycle detector...");
    offlineInfo =
        std::make_unique<OfflineCycleDetector>(constraints, nodeFactory);
    offlineInfo->run();
  }

  LOG_INFO("Building constraint graph...");
  ConstraintGraph constraintGraph(numNodes);
  buildConstraintGraph(constraintGraph, constraints, nodeFactory, ptsGraph);
  // The constraint vector is no longer needed during solving.
  constraints.clear();
  LOG_INFO("Constraint graph built, starting fixed-point iteration...");

  // oldPtsGraph[u] holds the subset of ptsGraph[u] already propagated along
  // all current copy/load/store edges of u.
  std::vector<PtsSetType> oldPtsGraph(numNodes);

  AndersWorkList workList1(numNodes), workList2(numNodes);
  AndersWorkList *currWorkList = &workList1, *nextWorkList = &workList2;

  // LCD candidate batch and persistent checked-edges set (PLDI 2007).
  std::vector<std::pair<NodeIndex, NodeIndex>> cycleCandidates;
  DenseSet<std::pair<NodeIndex, NodeIndex>> checkedEdges;

  size_t initEntries = 0;
  for (NodeIndex node = 0; node < numNodes; ++node) {
    if (!ptsGraph[node].isEmpty()) {
      ++initEntries;
      if (nodeFactory.getMergeTarget(node) == node &&
          (constraintGraph.getNodeWithIndex(node)->hasEdges() ||
           (EnableHCD &&
            offlineInfo->getCollapseTarget(node) !=
                AndersNodeFactory::InvalidIndex))) {
        currWorkList->enqueue(node);
      }
    }
  }
  LOG_INFO("Initializing work list from {} points-to graph entries...",
           initEntries);
  LOG_INFO("Work list initialized, entering main solving loop...");

  // Reusable online cycle detector (allocates dense SCC state once when LCD is
  // enabled and resets only visited entries between batches).
  std::unique_ptr<OnlineCycleDetectorT<PtsSetType>> onlineCycleDetector;
  if (EnableLCD) {
    onlineCycleDetector = std::make_unique<OnlineCycleDetectorT<PtsSetType>>(
        nodeFactory, constraintGraph, ptsGraph, oldPtsGraph, *nextWorkList,
        cycleCandidates);
  }

  // Reusable scratch buffers to avoid heap allocations in the hot loop.
  PtsSetType delta;
  PtsSetType bulkUnion;
  llvm::SparseBitVector<512> deltaReps;
  SmallVector<std::pair<NodeIndex, NodeIndex>, 16> edgeUpdates;

  unsigned iterCount = 0;
  while (!currWorkList->isEmpty()) {
    ++NumIterations;
    if (++iterCount == 1 || iterCount % 100 == 0) {
      LOG_INFO("Solving iteration {}", iterCount);
    }

    if (EnableLCD && !cycleCandidates.empty()) {
      NumCyclesDetected += cycleCandidates.size();
      onlineCycleDetector->setNextWorkList(*nextWorkList);
      onlineCycleDetector->run();
      cycleCandidates.clear();
    }

    unsigned nodeCount = 0;
    while (!currWorkList->isEmpty()) {
      NodeIndex node = currWorkList->dequeue();
      if (iterCount == 1 && ++nodeCount % 2000 == 0) {
        LOG_INFO("    - Processed {} nodes in iteration 1", nodeCount);
      }
      ++NumWorkListProcessed;
      node = nodeFactory.getMergeTarget(node);

      if (ptsGraph[node].isEmpty())
        continue;

      delta.setDifference(ptsGraph[node], oldPtsGraph[node]);
      if (delta.isEmpty())
        continue;

      // HCD: check if `node` has an offline collapse target before marking
      // `delta` as propagated, so any collapse into `ctRep` preserves delta.
      if (EnableHCD) {
        NodeIndex collapseTarget = offlineInfo->getCollapseTarget(node);
        if (collapseTarget != AndersNodeFactory::InvalidIndex) {
          NodeIndex ctRep = nodeFactory.getMergeTarget(collapseTarget);
          bool mergeSelf = false;
          bool mergedIntoNode = false;
          for (auto v : delta) {
            NodeIndex vRep = nodeFactory.getMergeTarget(static_cast<NodeIndex>(v));
            if (vRep == node) {
              mergeSelf = true;
              continue;
            }
            if (ctRep == node)
              mergedIntoNode = true;
            collapseNodes(ctRep, vRep, nodeFactory, ptsGraph, oldPtsGraph,
                          constraintGraph, *nextWorkList);
            ctRep = nodeFactory.getMergeTarget(ctRep);
          }

          if (mergeSelf) {
            collapseNodes(ctRep, node, nodeFactory, ptsGraph, oldPtsGraph,
                          constraintGraph, *nextWorkList);
            if (ctRep != node) {
              nextWorkList->enqueue(ctRep);
              continue;
            }
          }
          if (mergedIntoNode) {
            delta.setDifference(ptsGraph[node], oldPtsGraph[node]);
            if (delta.isEmpty())
              continue;
          }
        }
      }

      ConstraintGraphNode *cNode = constraintGraph.getNodeWithIndex(node);
      if (!cNode->hasEdges())
        continue;

      // Mark `delta` as propagated along all current edges of `node`.
      oldPtsGraph[node].unionWith(delta);

      // Process complex (load/store) edges only for newly arrived targets in
      // `delta`. Merging copy edges via SparseBitVector OR and aggregating the
      // points-to union reduces O(|delta| * |edges|) to O(|delta| + |edges|).
      if (cNode->hasLoads() || cNode->hasStores()) {
        deltaReps.clear();
        for (auto v : delta)
          deltaReps.set(nodeFactory.getMergeTarget(static_cast<NodeIndex>(v)));

        if (cNode->hasLoads()) {
          edgeUpdates.clear();
          for (auto dst : cNode->loads()) {
            NodeIndex tgtNode = nodeFactory.getMergeTarget(dst);
            if (tgtNode != dst)
              edgeUpdates.emplace_back(dst, tgtNode);
          }
          for (const auto &upd : edgeUpdates)
            cNode->replaceLoadEdge(upd.first, upd.second);

          const auto &loadEdgeSet = cNode->getLoadEdges();
          bulkUnion.clear();
          for (NodeIndex vRep : deltaReps) {
            ++NumLoadEdgesProcessed;
            ConstraintGraphNode *vNode = constraintGraph.getNodeWithIndex(vRep);
            if (vNode->mergeCopyEdges(loadEdgeSet, vRep)) {
              ++NumCopyEdgesAdded;
              if (!ptsGraph[vRep].isEmpty())
                bulkUnion.unionWith(ptsGraph[vRep]);
            }
          }
          if (!bulkUnion.isEmpty()) {
            for (NodeIndex tgtNode : loadEdgeSet) {
              ++NumPtsSetUnions;
              if (ptsGraph[tgtNode].unionWith(bulkUnion))
                nextWorkList->enqueue(tgtNode);
            }
          }
        }

        if (cNode->hasStores()) {
          edgeUpdates.clear();
          for (auto dst : cNode->stores()) {
            NodeIndex tgtNode = nodeFactory.getMergeTarget(dst);
            if (tgtNode != dst)
              edgeUpdates.emplace_back(dst, tgtNode);
          }
          for (const auto &upd : edgeUpdates)
            cNode->replaceStoreEdge(upd.first, upd.second);

          const auto &storeEdgeSet = cNode->getStoreEdges();
          bulkUnion.clear();
          for (NodeIndex tgtNode : storeEdgeSet) {
            ++NumStoreEdgesProcessed;
            ConstraintGraphNode *tNode =
                constraintGraph.getNodeWithIndex(tgtNode);
            if (tNode->mergeCopyEdges(deltaReps, tgtNode)) {
              ++NumCopyEdgesAdded;
              if (!ptsGraph[tgtNode].isEmpty())
                bulkUnion.unionWith(ptsGraph[tgtNode]);
            }
          }
          if (!bulkUnion.isEmpty()) {
            for (NodeIndex vRep : deltaReps) {
              ++NumPtsSetUnions;
              if (ptsGraph[vRep].unionWith(bulkUnion))
                nextWorkList->enqueue(vRep);
            }
          }
        }
      }

      // Propagate `delta` along copy edges.
      if (cNode->hasCopies()) {
        edgeUpdates.clear();
        for (auto dst : *cNode) {
          NodeIndex tgtNode = nodeFactory.getMergeTarget(dst);
          if (node == tgtNode) {
            edgeUpdates.emplace_back(dst, AndersNodeFactory::InvalidIndex);
            continue;
          }
          auto &tgtPtsSet = ptsGraph[tgtNode];
          ++NumPtsSetUnions;
          bool isChanged = tgtPtsSet.unionWith(delta);

          if (isChanged) {
            nextWorkList->enqueue(tgtNode);
          } else if (EnableLCD) {
            auto edgePair = std::make_pair(node, tgtNode);
            if (!checkedEdges.count(edgePair) && ptsGraph[node] == tgtPtsSet) {
              checkedEdges.insert(edgePair);
              cycleCandidates.push_back(edgePair);
            }
          }

          if (tgtNode != dst)
            edgeUpdates.emplace_back(dst, tgtNode);
        }
        for (const auto &upd : edgeUpdates) {
          if (upd.second == AndersNodeFactory::InvalidIndex)
            cNode->removeCopyEdge(upd.first);
          else
            cNode->replaceCopyEdge(upd.first, upd.second);
        }
      }
    }

    std::swap(currWorkList, nextWorkList);
  }

  LOG_INFO("Constraint solving loop completed, calculating statistics...");

  unsigned long long totalPtsSetSize = 0;
  unsigned maxSize = 0;
  size_t numNonEmptyPtsSets = 0;

  for (const auto &pts : ptsGraph) {
    if (pts.isEmpty())
      continue;
    unsigned size = pts.getSize();
    if (size > 0) {
      totalPtsSetSize += size;
      ++numNonEmptyPtsSets;
      if (size > maxSize)
        maxSize = size;
    }
  }

  stats.numSolvingIterations = iterCount;
  stats.numNonEmptyPtsSets = numNonEmptyPtsSets;
  stats.maxPtsSetSize = maxSize;
  stats.avgPtsSetSize =
      numNonEmptyPtsSets > 0
          ? static_cast<unsigned>(totalPtsSetSize / numNonEmptyPtsSets)
          : 0;

  MaxPtsSetSize = maxSize;
  if (numNonEmptyPtsSets > 0)
    AvgPtsSetSize = totalPtsSetSize / numNonEmptyPtsSets;
}

void Andersen::solveConstraints() {
  if (ptsSetImpl == PtsSetImpl::SPARSE_BITVECTOR) {
    bddPtsGraph.clear();
    solveConstraintsImpl(sparsePtsGraph);
  } else {
    sparsePtsGraph.clear();
    solveConstraintsImpl(bddPtsGraph);
  }
}
