/**
 * @file ConstraintSolving.cpp
 * @brief Constraint solving phase using worklist-based delta propagation.
 *
 * This file implements the constraint solving algorithm that propagates
 * points-to information through the constraint graph. Uses a flat CSR graph
 * for offline HCD, exact load/store hub factoring to eliminate bipartite
 * K_{M,N} edge blowup, dense node arrays, difference (delta) propagation, and
 * optional cycle detection (HCD: Hybrid Cycle Detection, LCD: Lazy Cycle
 * Detection).
 *
 * @author rainoftime
 */
#include "Alias/InclusionBased/SparrowAA/Andersen.h"
#include "Alias/InclusionBased/SparrowAA/CycleDetector.h"
#include "Alias/InclusionBased/SparrowAA/Log.h"

#include <algorithm>
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
// Because `nodes` is sized once to `totalNodes`, pointers to nodes never
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

// Lightweight flat CSR graph for OfflineCycleDetector (HCD).
// Builds in O(|V| + |E|) time and uses 4 bytes per edge instead of linked-list
// SparseBitVector insertions.
class CSRGraphNode {
private:
  NodeIndex idx = 0;
  const NodeIndex *beginPtr = nullptr;
  const NodeIndex *endPtr = nullptr;

public:
  using iterator = const NodeIndex *;

  CSRGraphNode() = default;
  CSRGraphNode(NodeIndex i, const NodeIndex *b, const NodeIndex *e)
      : idx(i), beginPtr(b), endPtr(e) {}

  NodeIndex getNodeIndex() const { return idx; }
  iterator begin() const { return beginPtr; }
  iterator end() const { return endPtr; }
};

class CSRGraph {
private:
  std::vector<NodeIndex> targets;
  std::vector<CSRGraphNode> nodes;
  std::vector<NodeIndex> activeNodes;

public:
  using iterator = std::vector<CSRGraphNode>::iterator;
  using const_iterator = std::vector<CSRGraphNode>::const_iterator;

  CSRGraph(unsigned numNodes,
           const std::vector<std::pair<NodeIndex, NodeIndex>> &edges) {
    std::vector<uint32_t> degree(numNodes, 0);
    for (const auto &e : edges)
      ++degree[e.first];

    std::vector<uint32_t> offsets(numNodes + 1, 0);
    for (unsigned i = 0; i < numNodes; ++i)
      offsets[i + 1] = offsets[i] + degree[i];

    targets.resize(edges.size());
    std::vector<uint32_t> cursor = offsets;
    for (const auto &e : edges)
      targets[cursor[e.first]++] = e.second;

    const NodeIndex *base = targets.data();
    nodes.reserve(numNodes);
    for (unsigned i = 0; i < numNodes; ++i) {
      nodes.emplace_back(i, base + offsets[i], base + offsets[i + 1]);
      if (offsets[i] != offsets[i + 1])
        activeNodes.push_back(i);
    }
  }

  CSRGraphNode *getNode(NodeIndex idx) { return &nodes[idx]; }
  const std::vector<NodeIndex> &getActiveNodes() const { return activeNodes; }

  void releaseMemory() {
    std::vector<NodeIndex>().swap(targets);
    std::vector<CSRGraphNode>().swap(nodes);
    std::vector<NodeIndex>().swap(activeNodes);
  }

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

// Specialize AndersGraphTraits for CSRGraph
template <> class AndersGraphTraits<CSRGraph> {
public:
  using NodeType = CSRGraphNode;
  using NodeIterator = CSRGraph::iterator;
  using ChildIterator = CSRGraphNode::iterator;

  static inline ChildIterator child_begin(const NodeType *n) {
    return n->begin();
  }
  static inline ChildIterator child_end(const NodeType *n) { return n->end(); }

  static inline NodeIterator node_begin(CSRGraph *g) { return g->begin(); }
  static inline NodeIterator node_end(CSRGraph *g) { return g->end(); }
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
    ptsGraph[src].clearAndRelease();
  }

  bool dstHadEdges = constraintGraph.getNodeWithIndex(dst)->hasEdges();
  bool edgesMerged = constraintGraph.mergeNodes(dst, src);
  bool oldShrunk = false;
  if (edgesMerged) {
    // Only elements already propagated along BOTH dst's and src's edges remain
    // propagated along the merged edge set.
    oldShrunk = oldPtsGraph[dst].intersectAssign(oldPtsGraph[src]);
  }
  oldPtsGraph[src].clearAndRelease();
  constraintGraph.deleteNode(src);

  if ((ptsChanged || oldShrunk || (edgesMerged && !dstHadEdges)) &&
      !ptsGraph[dst].isEmpty())
    nextWorkList.enqueue(dst);
}

// Offline portion of HCD (Hybrid Cycle Detection, PLDI 2007) backed by a
// linear-time CSR graph.
class OfflineCycleDetector : public CycleDetector<CSRGraph> {
private:
  AndersNodeFactory &nodeFactory;
  unsigned numOrigNodes;
  std::unique_ptr<CSRGraph> offlineGraph;
  DenseMap<NodeIndex, NodeIndex> collapseMap;
  std::vector<std::pair<NodeIndex, NodeIndex>> mergeList;
  std::vector<NodeIndex> sccNodes;

  NodeIndex getRefNodeIndex(NodeIndex n) const { return n + numOrigNodes; }

  NodeType *getRep(NodeIndex idx) override { return offlineGraph->getNode(idx); }

  void processNodeOnCycle(const NodeType *node,
                          const NodeType *repNode) override {
    (void)repNode;
    sccNodes.push_back(node->getNodeIndex());
  }

  void processCycleRepNode(const NodeType *node) override {
    if (sccNodes.empty())
      return;

    sccNodes.push_back(node->getNodeIndex());

    NodeIndex repNode = AndersNodeFactory::InvalidIndex;
    for (NodeIndex idx : sccNodes) {
      if (idx < numOrigNodes && idx < repNode)
        repNode = idx;
    }
    if (repNode == AndersNodeFactory::InvalidIndex) {
      sccNodes.clear();
      return;
    }

    for (NodeIndex cycleNode : sccNodes) {
      if (cycleNode == repNode)
        continue;
      if (cycleNode >= numOrigNodes)
        collapseMap[cycleNode - numOrigNodes] = repNode;
      else
        mergeList.emplace_back(cycleNode, repNode);
    }

    sccNodes.clear();
  }

public:
  OfflineCycleDetector(const std::vector<AndersConstraint> &cs,
                       AndersNodeFactory &n)
      : nodeFactory(n), numOrigNodes(n.getNumNodes()) {
    std::vector<std::pair<NodeIndex, NodeIndex>> edges;
    edges.reserve(cs.size());
    for (auto const &c : cs) {
      if (c.getType() == AndersConstraint::ADDR_OF)
        continue;
      NodeIndex srcTgt = nodeFactory.getMergeTarget(c.getSrc());
      NodeIndex dstTgt = nodeFactory.getMergeTarget(c.getDest());
      switch (c.getType()) {
      case AndersConstraint::ADDR_OF:
        break;
      case AndersConstraint::LOAD:
        edges.emplace_back(getRefNodeIndex(srcTgt), dstTgt);
        break;
      case AndersConstraint::STORE:
        edges.emplace_back(srcTgt, getRefNodeIndex(dstTgt));
        break;
      case AndersConstraint::COPY:
        if (srcTgt != dstTgt)
          edges.emplace_back(srcTgt, dstTgt);
        break;
      }
    }
    offlineGraph = std::make_unique<CSRGraph>(numOrigNodes * 2, edges);
    reserveSCCState(numOrigNodes * 2);
  }

  void run() override {
    runOnNodeList(offlineGraph->getActiveNodes());

    for (const auto &mapping : mergeList)
      nodeFactory.mergeNode(mapping.second, mapping.first);

    mergeList.clear();
    offlineGraph->releaseMemory();
    offlineGraph.reset();
    releaseSCCMemory();
  }

  NodeIndex getCollapseTarget(NodeIndex n) const {
    auto itr = collapseMap.find(n);
    if (itr == collapseMap.end())
      return AndersNodeFactory::InvalidIndex;
    return itr->second;
  }
};

// Build the constraint graph with exact load/store hub factoring:
// - If a pointer `p` has L >= 2 distinct load targets l_1..l_L, introduce one
//   relay node H_L with `H_L = *p` and `l_i = H_L`.
// - If a pointer `p` has M >= 2 distinct store sources s_1..s_M, introduce one
//   relay node H_S with `H_S = s_i` and `*p = H_S`.
// This reduces bipartite K_{M,N} copy-edge blowups during solving to O(M + N)
// with mathematically identical points-to sets for all program nodes.
// Copy edges are also sorted by (src, dst) so SparseBitVector insertions are
// strictly ascending (O(1) amortized per edge).
template <typename PtsSetType>
std::unique_ptr<ConstraintGraph>
buildFactoredConstraintGraph(std::vector<AndersConstraint> &constraints,
                             AndersNodeFactory &nodeFactory,
                             std::vector<PtsSetType> &ptsGraph,
                             std::vector<NodeIndex> &objIdToNode) {
  auto packPair = [](NodeIndex hi, NodeIndex lo) -> uint64_t {
    return (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
  };

  const unsigned origNumNodes = nodeFactory.getNumNodes();
  const NodeIndex univObjNode = nodeFactory.getUniversalObjNode();
  const NodeIndex nullObjNode = nodeFactory.getNullObjectNode();

  std::vector<uint64_t> addrOfPairs;
  std::vector<NodeIndex> uniqueObjs;
  std::vector<uint64_t> copyPairs;
  std::vector<uint64_t> loadPairs;
  std::vector<uint64_t> storePairs;
  copyPairs.reserve(constraints.size() / 2);

  for (auto const &c : constraints) {
    NodeIndex srcTgt = nodeFactory.getMergeTarget(c.getSrc());
    NodeIndex dstTgt = nodeFactory.getMergeTarget(c.getDest());
    switch (c.getType()) {
    case AndersConstraint::ADDR_OF:
      // Preserve exact original object node c.getSrc() in points-to sets.
      addrOfPairs.push_back(packPair(dstTgt, c.getSrc()));
      if (c.getSrc() != univObjNode && c.getSrc() != nullObjNode)
        uniqueObjs.push_back(c.getSrc());
      break;
    case AndersConstraint::LOAD:
      // Group by dereferenced pointer `srcTgt`.
      loadPairs.push_back(packPair(srcTgt, dstTgt));
      break;
    case AndersConstraint::STORE:
      // Group by dereferenced pointer `dstTgt`.
      storePairs.push_back(packPair(dstTgt, srcTgt));
      break;
    case AndersConstraint::COPY:
      if (srcTgt != dstTgt)
        copyPairs.push_back(packPair(srcTgt, dstTgt));
      break;
    }
  }
  // Free raw constraints early to reduce peak RSS.
  std::vector<AndersConstraint>().swap(constraints);

  // Build compact object ID table: 0 = UniversalObj, 1 = NullObject,
  // 2.. = sorted distinct ADDR_OF object nodes. Compact IDs shrink every
  // bitset's universe by ~10-30x (only address-taken objects occupy bits).
  std::sort(uniqueObjs.begin(), uniqueObjs.end());
  uniqueObjs.erase(std::unique(uniqueObjs.begin(), uniqueObjs.end()),
                   uniqueObjs.end());
  objIdToNode.clear();
  objIdToNode.reserve(uniqueObjs.size() + 2);
  objIdToNode.push_back(univObjNode); // ID 0 (UniversalObjCompactId)
  objIdToNode.push_back(nullObjNode); // ID 1 (NullObjCompactId)
  std::vector<uint32_t> nodeToObjId(origNumNodes, UINT32_MAX);
  nodeToObjId[univObjNode] = 0;
  nodeToObjId[nullObjNode] = 1;
  for (NodeIndex obj : uniqueObjs) {
    nodeToObjId[obj] = static_cast<uint32_t>(objIdToNode.size());
    objIdToNode.push_back(obj);
  }
  std::vector<NodeIndex>().swap(uniqueObjs);

  auto sortAndUnique = [](std::vector<uint64_t> &vec) {
    std::sort(vec.begin(), vec.end());
    vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
  };

  sortAndUnique(loadPairs);
  sortAndUnique(storePairs);

  std::vector<std::pair<NodeIndex, NodeIndex>> factoredLoads;
  factoredLoads.reserve(loadPairs.size());
  for (size_t i = 0, n = loadPairs.size(); i < n;) {
    NodeIndex ptr = static_cast<NodeIndex>(loadPairs[i] >> 32);
    size_t j = i + 1;
    while (j < n && static_cast<NodeIndex>(loadPairs[j] >> 32) == ptr)
      ++j;
    if (j - i == 1) {
      factoredLoads.emplace_back(
          ptr, static_cast<NodeIndex>(loadPairs[i] & 0xFFFFFFFFu));
    } else {
      NodeIndex hub = nodeFactory.createValueNode(nullptr, nullptr);
      factoredLoads.emplace_back(ptr, hub);
      for (size_t k = i; k < j; ++k) {
        NodeIndex dst = static_cast<NodeIndex>(loadPairs[k] & 0xFFFFFFFFu);
        if (hub != dst)
          copyPairs.push_back(packPair(hub, dst));
      }
    }
    i = j;
  }
  std::vector<uint64_t>().swap(loadPairs);

  std::vector<std::pair<NodeIndex, NodeIndex>> factoredStores;
  factoredStores.reserve(storePairs.size());
  for (size_t i = 0, n = storePairs.size(); i < n;) {
    NodeIndex ptr = static_cast<NodeIndex>(storePairs[i] >> 32);
    size_t j = i + 1;
    while (j < n && static_cast<NodeIndex>(storePairs[j] >> 32) == ptr)
      ++j;
    if (j - i == 1) {
      factoredStores.emplace_back(
          ptr, static_cast<NodeIndex>(storePairs[i] & 0xFFFFFFFFu));
    } else {
      NodeIndex hub = nodeFactory.createValueNode(nullptr, nullptr);
      factoredStores.emplace_back(ptr, hub);
      for (size_t k = i; k < j; ++k) {
        NodeIndex src = static_cast<NodeIndex>(storePairs[k] & 0xFFFFFFFFu);
        if (src != hub)
          copyPairs.push_back(packPair(src, hub));
      }
    }
    i = j;
  }
  std::vector<uint64_t>().swap(storePairs);

  const unsigned totalNodes = nodeFactory.getNumNodes();
  ptsGraph.clear();
  ptsGraph.resize(totalNodes);

  // Remap ADDR_OF object nodes to compact IDs before sorting so insertions into
  // each points-to set are strictly ascending (O(1) chunk append).
  for (uint64_t &packed : addrOfPairs) {
    NodeIndex dst = static_cast<NodeIndex>(packed >> 32);
    NodeIndex obj = static_cast<NodeIndex>(packed & 0xFFFFFFFFu);
    packed = packPair(dst, nodeToObjId[obj]);
  }
  std::vector<uint32_t>().swap(nodeToObjId);
  sortAndUnique(addrOfPairs);
  for (uint64_t packed : addrOfPairs) {
    NodeIndex dst = static_cast<NodeIndex>(packed >> 32);
    unsigned compactObjId = static_cast<unsigned>(packed & 0xFFFFFFFFu);
    ptsGraph[dst].insert(compactObjId);
  }
  std::vector<uint64_t>().swap(addrOfPairs);

  auto cGraph = std::make_unique<ConstraintGraph>(totalNodes);

  // Inserting sorted (src, dst) pairs keeps SparseBitVector insertions
  // monotonically increasing per src node (O(1) amortized).
  sortAndUnique(copyPairs);
  for (uint64_t packed : copyPairs) {
    NodeIndex src = static_cast<NodeIndex>(packed >> 32);
    NodeIndex dst = static_cast<NodeIndex>(packed & 0xFFFFFFFFu);
    cGraph->insertCopyEdge(src, dst);
  }
  std::vector<uint64_t>().swap(copyPairs);

  for (const auto &e : factoredLoads)
    cGraph->insertLoadEdge(e.first, e.second);
  for (const auto &e : factoredStores)
    cGraph->insertStoreEdge(e.first, e.second);

  return cGraph;
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
  const unsigned origNumNodes = nodeFactory.getNumNodes();

  // Only build and run OfflineCycleDetector when HCD is enabled.
  std::unique_ptr<OfflineCycleDetector> offlineInfo;
  if (EnableHCD) {
    LOG_INFO("Creating offline cycle detector...");
    offlineInfo =
        std::make_unique<OfflineCycleDetector>(constraints, nodeFactory);
    offlineInfo->run();
  }

  LOG_INFO("Building constraint graph...");
  auto constraintGraphPtr = buildFactoredConstraintGraph(
      constraints, nodeFactory, ptsGraph, objIdToNode);
  ConstraintGraph &constraintGraph = *constraintGraphPtr;
  const unsigned totalNodes = nodeFactory.getNumNodes();
  LOG_INFO("Constraint graph built ({} nodes incl. {} relay hubs, {} compact "
           "objects), starting fixed-point iteration...",
           totalNodes, totalNodes - origNumNodes, objIdToNode.size());

  // oldPtsGraph[u] holds the subset of ptsGraph[u] already propagated along
  // all current copy/load/store edges of u.
  std::vector<PtsSetType> oldPtsGraph(totalNodes);

  AndersWorkList workList1(totalNodes), workList2(totalNodes);
  AndersWorkList *currWorkList = &workList1, *nextWorkList = &workList2;

  // LCD candidate batch and persistent checked-edges set (PLDI 2007).
  std::vector<std::pair<NodeIndex, NodeIndex>> cycleCandidates;
  DenseSet<std::pair<NodeIndex, NodeIndex>> checkedEdges;

  // Order Iteration 1 seeds in reverse postorder of the copy graph so upstream
  // copy sources propagate before downstream destinations.
  std::vector<NodeIndex> initSeeds;
  size_t initEntries = 0;
  for (NodeIndex node = 0; node < totalNodes; ++node) {
    if (!ptsGraph[node].isEmpty()) {
      ++initEntries;
      if (nodeFactory.getMergeTarget(node) == node &&
          (constraintGraph.getNodeWithIndex(node)->hasEdges() ||
           (EnableHCD && node < origNumNodes &&
            offlineInfo->getCollapseTarget(node) !=
                AndersNodeFactory::InvalidIndex))) {
        initSeeds.push_back(node);
      }
    }
  }
  {
    std::vector<uint8_t> isSeed(totalNodes, 0);
    for (NodeIndex n : initSeeds)
      isSeed[n] = 1;
    std::vector<uint8_t> visited(totalNodes, 0);
    std::vector<NodeIndex> postOrder;
    postOrder.reserve(initSeeds.size());
    struct DFSFrame {
      NodeIndex node;
      ConstraintGraphNode::iterator it;
      ConstraintGraphNode::iterator end;
    };
    std::vector<DFSFrame> stack;
    for (NodeIndex seed : initSeeds) {
      if (visited[seed])
        continue;
      visited[seed] = 1;
      ConstraintGraphNode *cn = constraintGraph.getNodeWithIndex(seed);
      stack.push_back({seed, cn->begin(), cn->end()});
      while (!stack.empty()) {
        DFSFrame &frame = stack.back();
        if (frame.it != frame.end) {
          NodeIndex succ = nodeFactory.getMergeTarget(*frame.it);
          ++frame.it;
          if (!visited[succ]) {
            visited[succ] = 1;
            ConstraintGraphNode *scn = constraintGraph.getNodeWithIndex(succ);
            stack.push_back({succ, scn->begin(), scn->end()});
          }
        } else {
          if (isSeed[frame.node])
            postOrder.push_back(frame.node);
          stack.pop_back();
        }
      }
    }
    for (auto it = postOrder.rbegin(); it != postOrder.rend(); ++it)
      currWorkList->enqueue(*it);
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
  std::vector<NodeIndex> deltaRepVec;
  llvm::SparseBitVector<512> deltaReps;
  SmallVector<std::pair<NodeIndex, NodeIndex>, 16> edgeUpdates;
  static constexpr size_t kPeriodicLCDThreshold = 1024;

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

      ConstraintGraphNode *cNode = constraintGraph.getNodeWithIndex(node);
      NodeIndex collapseTarget = AndersNodeFactory::InvalidIndex;
      if (EnableHCD && node < origNumNodes)
        collapseTarget = offlineInfo->getCollapseTarget(node);

      // Skip sink nodes before computing `delta` or touching `oldPtsGraph`.
      if (!cNode->hasEdges() &&
          collapseTarget == AndersNodeFactory::InvalidIndex)
        continue;

      const PtsSetType *effDeltaPtr = nullptr;
      if (collapseTarget != AndersNodeFactory::InvalidIndex) {
        // HCD path: materialize `delta` first because collapsing into `ctRep`
        // (which may equal `node`) can mutate `ptsGraph[node]`.
        delta.setDifference(ptsGraph[node], oldPtsGraph[node]);
        if (delta.isEmpty())
          continue;

        NodeIndex ctRep = nodeFactory.getMergeTarget(collapseTarget);
        bool mergeSelf = false;
        bool mergedIntoNode = false;
        for (auto objId : delta) {
          NodeIndex vRep = nodeFactory.getMergeTarget(objIdToNode[objId]);
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
        if (!cNode->hasEdges())
          continue;
        oldPtsGraph[node] = ptsGraph[node];
        effDeltaPtr = &delta;
      } else if (oldPtsGraph[node].isEmpty()) {
        // Fast path (100% of Iteration 1 and every node's first propagation):
        // share directly into oldPtsGraph[node] (O(1) CoW refcount bump;
        // oldPtsGraph is immutable throughout load/store/copy propagation).
        oldPtsGraph[node] = ptsGraph[node];
        effDeltaPtr = &oldPtsGraph[node];
      } else {
        delta.setDifference(ptsGraph[node], oldPtsGraph[node]);
        if (delta.isEmpty())
          continue;
        oldPtsGraph[node] = ptsGraph[node];
        effDeltaPtr = &delta;
      }

      const PtsSetType &effDelta = *effDeltaPtr;

      // Process complex (load/store) edges only for newly arrived targets in
      // `effDelta`. After hub factoring almost every node has at most 1 load
      // edge and 1 store edge, so the singleton fast-paths avoid full
      // SparseBitVector OR + reset scans.
      if (cNode->hasLoads() || cNode->hasStores()) {
        deltaRepVec.clear();
        for (auto objId : effDelta)
          deltaRepVec.push_back(nodeFactory.getMergeTarget(objIdToNode[objId]));
        if (deltaRepVec.size() > 1) {
          std::sort(deltaRepVec.begin(), deltaRepVec.end());
          deltaRepVec.erase(std::unique(deltaRepVec.begin(), deltaRepVec.end()),
                            deltaRepVec.end());
        }

        if (cNode->hasLoads()) {
          edgeUpdates.clear();
          NodeIndex singleTgt = AndersNodeFactory::InvalidIndex;
          unsigned loadCount = 0;
          for (auto dst : cNode->loads()) {
            NodeIndex tgtNode = nodeFactory.getMergeTarget(dst);
            if (tgtNode != dst)
              edgeUpdates.emplace_back(dst, tgtNode);
            singleTgt = tgtNode;
            ++loadCount;
          }
          for (const auto &upd : edgeUpdates)
            cNode->replaceLoadEdge(upd.first, upd.second);

          bulkUnion.clear();
          if (loadCount == 1) {
            for (NodeIndex vRep : deltaRepVec) {
              ++NumLoadEdgesProcessed;
              if (constraintGraph.insertCopyEdge(vRep, singleTgt)) {
                ++NumCopyEdgesAdded;
                if (!ptsGraph[vRep].isEmpty())
                  bulkUnion.unionWith(ptsGraph[vRep]);
              }
            }
            if (!bulkUnion.isEmpty()) {
              ++NumPtsSetUnions;
              if (ptsGraph[singleTgt].unionWith(bulkUnion))
                nextWorkList->enqueue(singleTgt);
            }
          } else {
            const auto &loadEdgeSet = cNode->getLoadEdges();
            for (NodeIndex vRep : deltaRepVec) {
              ++NumLoadEdgesProcessed;
              ConstraintGraphNode *vNode =
                  constraintGraph.getNodeWithIndex(vRep);
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
        }

        if (cNode->hasStores()) {
          edgeUpdates.clear();
          NodeIndex singleSrc = AndersNodeFactory::InvalidIndex;
          unsigned storeCount = 0;
          for (auto dst : cNode->stores()) {
            NodeIndex tgtNode = nodeFactory.getMergeTarget(dst);
            if (tgtNode != dst)
              edgeUpdates.emplace_back(dst, tgtNode);
            singleSrc = tgtNode;
            ++storeCount;
          }
          for (const auto &upd : edgeUpdates)
            cNode->replaceStoreEdge(upd.first, upd.second);

          if (storeCount == 1) {
            if (deltaRepVec.size() == 1) {
              NodeIndex vRep = deltaRepVec[0];
              ++NumStoreEdgesProcessed;
              if (constraintGraph.insertCopyEdge(singleSrc, vRep)) {
                ++NumCopyEdgesAdded;
                if (!ptsGraph[singleSrc].isEmpty()) {
                  ++NumPtsSetUnions;
                  if (ptsGraph[vRep].unionWith(ptsGraph[singleSrc]))
                    nextWorkList->enqueue(vRep);
                }
              }
            } else {
              deltaReps.clear();
              for (NodeIndex vRep : deltaRepVec)
                deltaReps.set(vRep);
              ++NumStoreEdgesProcessed;
              ConstraintGraphNode *sNode =
                  constraintGraph.getNodeWithIndex(singleSrc);
              if (sNode->mergeCopyEdges(deltaReps, singleSrc)) {
                ++NumCopyEdgesAdded;
                if (!ptsGraph[singleSrc].isEmpty()) {
                  const auto &srcPts = ptsGraph[singleSrc];
                  for (NodeIndex vRep : deltaRepVec) {
                    if (vRep == singleSrc)
                      continue;
                    ++NumPtsSetUnions;
                    if (ptsGraph[vRep].unionWith(srcPts))
                      nextWorkList->enqueue(vRep);
                  }
                }
              }
            }
          } else {
            deltaReps.clear();
            for (NodeIndex vRep : deltaRepVec)
              deltaReps.set(vRep);
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
              for (NodeIndex vRep : deltaRepVec) {
                ++NumPtsSetUnions;
                if (ptsGraph[vRep].unionWith(bulkUnion))
                  nextWorkList->enqueue(vRep);
              }
            }
          }
        }
      }

      // Propagate `effDelta` along copy edges.
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
          bool isChanged = tgtPtsSet.unionWith(effDelta);

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

      // Flush online cycle detection mid-iteration on large worklists so cycles
      // collapse before propagating huge points-to sets through the rest of the
      // wave.
      if (EnableLCD && cycleCandidates.size() >= kPeriodicLCDThreshold) {
        NumCyclesDetected += cycleCandidates.size();
        onlineCycleDetector->setNextWorkList(*nextWorkList);
        onlineCycleDetector->run();
        cycleCandidates.clear();
      }
    }

    std::swap(currWorkList, nextWorkList);
  }

  // Release solver-only structures before computing final statistics.
  onlineCycleDetector.reset();
  std::vector<PtsSetType>().swap(oldPtsGraph);
  constraintGraphPtr.reset();

  LOG_INFO("Constraint solving loop completed, calculating statistics...");

  unsigned long long totalPtsSetSize = 0;
  unsigned maxSize = 0;
  size_t numNonEmptyPtsSets = 0;
  std::vector<uint8_t> seenRep(totalNodes, 0);

  for (NodeIndex n = 0; n < origNumNodes; ++n) {
    NodeIndex rep = nodeFactory.getMergeTarget(n);
    if (seenRep[rep])
      continue;
    seenRep[rep] = 1;
    const auto &pts = ptsGraph[rep];
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
