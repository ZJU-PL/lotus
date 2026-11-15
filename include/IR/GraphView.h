/**
 * @file GraphView.h
 * @brief Non-owning filtered views over Lotus graphs (ICFG, SVFG, ...).
 *
 * A view keeps the source graph untouched and answers neighbour queries over a
 * retained node subset. Two semantics are available, and picking the right one
 * matters:
 *
 *  - **Induced** (default, `BridgeMode::Induced`): an edge is visible exactly
 *    when both endpoints are retained. This is the only sound semantics for a
 *    *value-flow* graph — a fabricated value-flow path is a fabricated
 *    dependence.
 *
 *  - **Contract** (`BridgeMode::Contract`): retained nodes are additionally
 *    *bridged* across runs of removed nodes, i.e. the removed nodes are
 *    contracted and the surviving edges are the composition of the paths that
 *    went through them. This is what *control-flow* consumers need
 *    (reachability, MHP, lockset, region formation): deleting a join block must
 *    not delete the control dependence that flows through it.
 *
 * Bridging is guarded by `ViewBridgingPolicy`, which is a compile-time trait:
 * enabled for the ICFG, disabled for the SVFG. The distinction is deliberate
 * and mirrors SlicedGraphs in SVF, where `SlicedICFGView` carries bridged
 * edges while `SlicedSVFGView` documents "there are deliberately no bridge
 * edges, since bridging would fabricate value-flow paths".
 */
#pragma once

#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFG.h"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::analysis {

/// Whether a graph may be viewed with bridging (node contraction).
///
/// Default is `false`: a graph type has to opt in explicitly. Only graphs whose
/// edges denote *control flow* may opt in; value-flow graphs must not.
template <typename GraphT> struct ViewBridgingPolicy : std::false_type {};

/// The ICFG is a control-flow graph, so contracting removed nodes is sound.
template <> struct ViewBridgingPolicy<ICFG> : std::true_type {};

/// How a filtered view treats paths through removed nodes.
enum class BridgeMode {
  /// Strict induced subgraph: an edge is visible iff both endpoints are kept.
  Induced,
  /// Induced subgraph plus synthetic edges that contract removed nodes.
  /// Only valid when `ViewBridgingPolicy<GraphT>::value` holds.
  Contract,
};

/// A non-owning view over a graph.
///
/// Nodes and edges remain owned by the source graph. With
/// `BridgeMode::Induced` the view is exactly the node-induced subgraph. With
/// `BridgeMode::Contract` it additionally exposes, for every retained node,
/// the retained nodes reachable through *only* removed nodes.
///
/// @tparam GraphT Source graph; iterating it must yield `(id, NodeT *)` pairs.
/// @tparam NodeT  Node type; must provide `getId()`, `getOutEdges()`,
///                `getInEdges()`.
/// @tparam EdgeT  Edge type; must provide `getSrcNode()`, `getDstNode()`.
template <typename GraphT, typename NodeT, typename EdgeT>
class FilteredGraphView {
public:
  using NodeSet = std::unordered_set<const NodeT *>;
  using NodeList = std::vector<const NodeT *>;
  using EdgeList = std::vector<const EdgeT *>;

  /// Builds a view from an explicit node membership.
  FilteredGraphView(const GraphT &graph, NodeSet retained,
                    BridgeMode mode = BridgeMode::Induced)
      : graph_(&graph), retained_(std::move(retained)) {
    initBridging(mode);
  }

  /// Builds a view from a node predicate.
  template <typename Predicate>
  FilteredGraphView(const GraphT &graph, Predicate keep,
                    BridgeMode mode = BridgeMode::Induced)
      : graph_(&graph) {
    for (const auto &entry : graph) {
      const NodeT *node = entry.second;
      if (node && keep(*node))
        retained_.insert(node);
    }
    initBridging(mode);
  }

  const GraphT &source() const { return *graph_; }

  /// The mode this view was built with. `BridgeMode::Contract` silently
  /// degrades to `Induced` for graph types that disallow bridging.
  BridgeMode bridgeMode() const { return mode_; }

  bool contains(const NodeT *node) const {
    return node && retained_.count(node) != 0;
  }

  bool contains(const EdgeT *edge) const {
    return edge && contains(edge->getSrcNode()) && contains(edge->getDstNode());
  }

  std::size_t nodeCount() const { return retained_.size(); }

  std::size_t edgeCount() const {
    std::size_t count = 0;
    for (const NodeT *node : retained_)
      for (const EdgeT *edge : node->getOutEdges())
        count += contains(edge) ? 1U : 0U;
    return count;
  }

  /// Number of synthetic edges created by contracting removed nodes.
  std::size_t bridgeCount() const { return bridgeCount_; }

  bool hasBridges() const { return bridgeCount_ != 0; }

  const NodeSet &nodes() const { return retained_; }

  /// Real edges only. Order follows the source graph's edge set.
  EdgeList outgoing(const NodeT *node) const {
    EdgeList result;
    if (!contains(node))
      return result;
    for (const EdgeT *edge : node->getOutEdges())
      if (contains(edge))
        result.push_back(edge);
    return result;
  }

  /// Real edges only. Order follows the source graph's edge set.
  EdgeList incoming(const NodeT *node) const {
    EdgeList result;
    if (!contains(node))
      return result;
    for (const EdgeT *edge : node->getInEdges())
      if (contains(edge))
        result.push_back(edge);
    return result;
  }

  /// Real successors only (induced semantics).
  NodeList successors(const NodeT *node) const {
    NodeList result;
    for (const EdgeT *edge : outgoing(node))
      result.push_back(edge->getDstNode());
    return result;
  }

  /// Real predecessors only (induced semantics).
  NodeList predecessors(const NodeT *node) const {
    NodeList result;
    for (const EdgeT *edge : incoming(node))
      result.push_back(edge->getSrcNode());
    return result;
  }

  /// Retained nodes reachable from @p node through removed nodes only.
  /// Returns `nullptr` when there is no such path.
  const NodeList *bridgedSuccessors(const NodeT *node) const {
    const auto it = bridgedSucc_.find(node);
    return it == bridgedSucc_.end() ? nullptr : &it->second;
  }

  /// Reverse of `bridgedSuccessors`.
  const NodeList *bridgedPredecessors(const NodeT *node) const {
    const auto it = bridgedPred_.find(node);
    return it == bridgedPred_.end() ? nullptr : &it->second;
  }

  /// Successors in the *contracted* graph: real successors plus bridges.
  /// Deduplicated and ordered by node id so results are reproducible.
  NodeList condensedSuccessors(const NodeT *node) const {
    NodeList result = successors(node);
    if (const NodeList *bridge = bridgedSuccessors(node))
      result.insert(result.end(), bridge->begin(), bridge->end());
    return sortUnique(result);
  }

  /// Predecessors in the *contracted* graph: real plus bridges.
  /// Deduplicated and ordered by node id so results are reproducible.
  NodeList condensedPredecessors(const NodeT *node) const {
    NodeList result = predecessors(node);
    if (const NodeList *bridge = bridgedPredecessors(node))
      result.insert(result.end(), bridge->begin(), bridge->end());
    return sortUnique(result);
  }

private:
  /// Deterministic order: node id, then pointer as a tie-break (ids are unique
  /// within a graph, so the tie-break never fires in practice).
  static NodeList sortUnique(NodeList nodes) {
    const auto less = [](const NodeT *lhs, const NodeT *rhs) {
      if (lhs->getId() != rhs->getId())
        return lhs->getId() < rhs->getId();
      return lhs < rhs;
    };
    std::sort(nodes.begin(), nodes.end(), less);
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    return nodes;
  }

  void initBridging(BridgeMode mode) {
    mode_ = BridgeMode::Induced;
    if constexpr (!ViewBridgingPolicy<GraphT>::value) {
      // Value-flow-like graph: bridging would invent edges. Refuse quietly
      // rather than assert so a caller cannot turn a typo into a crash.
      (void)mode;
      return;
    } else {
      if (mode == BridgeMode::Contract) {
        mode_ = BridgeMode::Contract;
        buildBridges();
      }
    }
  }

  /// Contracts every removed node: for each retained node, collect the retained
  /// nodes reachable through a path of removed nodes.
  ///
  /// Computed as a monotone fixpoint over the removed subgraph, so cycles made
  /// purely of removed nodes terminate (a NaiveDFS would not).
  void buildBridges() {
    // 1. Partition the graph.
    std::vector<const NodeT *> removed;
    for (const auto &entry : *graph_) {
      const NodeT *node = entry.second;
      if (node && !retained_.count(node))
        removed.push_back(node);
    }
    if (removed.empty())
      return;

    std::unordered_set<const NodeT *> removedSet(removed.begin(),
                                                 removed.end());

    // 2. Reverse adjacency restricted to removed nodes, so a growing
    //    reachability set can wake its removed predecessors.
    std::unordered_map<const NodeT *, NodeList> removedPreds;
    for (const NodeT *node : removed)
      for (const EdgeT *edge : node->getOutEdges()) {
        const NodeT *dst = edge->getDstNode();
        if (removedSet.count(dst))
          removedPreds[dst].push_back(node);
      }

    // 3. reach[d] = retained nodes reachable from removed node d through
    //    removed nodes only.
    std::unordered_map<const NodeT *, NodeSet> reach;
    NodeList worklist = removed;
    while (!worklist.empty()) {
      const NodeT *node = worklist.back();
      worklist.pop_back();

      NodeSet &targets = reach[node];
      const std::size_t before = targets.size();
      for (const EdgeT *edge : node->getOutEdges()) {
        const NodeT *dst = edge->getDstNode();
        if (retained_.count(dst))
          targets.insert(dst);
        else {
          const auto it = reach.find(dst);
          if (it != reach.end())
            targets.insert(it->second.begin(), it->second.end());
        }
      }
      if (targets.size() == before)
        continue;

      const auto preds = removedPreds.find(node);
      if (preds != removedPreds.end())
        worklist.insert(worklist.end(), preds->second.begin(),
                        preds->second.end());
    }

    // 4. A retained node bridges to whatever its removed successors reach.
    for (const NodeT *node : retained_) {
      NodeList bridged;
      for (const EdgeT *edge : node->getOutEdges()) {
        const NodeT *dst = edge->getDstNode();
        if (!dst || retained_.count(dst))
          continue;
        const auto it = reach.find(dst);
        if (it == reach.end())
          continue;
        bridged.insert(bridged.end(), it->second.begin(), it->second.end());
      }
      if (bridged.empty())
        continue;
      bridged = sortUnique(std::move(bridged));
      bridgeCount_ += bridged.size();
      for (const NodeT *dst : bridged)
        bridgedPred_[dst].push_back(node);
      bridgedSucc_.emplace(node, std::move(bridged));
    }

    // Keep the reverse map deterministic as well.
    for (auto &entry : bridgedPred_)
      entry.second = sortUnique(std::move(entry.second));
  }

  const GraphT *graph_;
  NodeSet retained_;
  BridgeMode mode_ = BridgeMode::Induced;
  std::size_t bridgeCount_ = 0;
  /// Retained node -> retained nodes reached by contracting removed nodes.
  std::unordered_map<const NodeT *, NodeList> bridgedSucc_;
  /// Reverse of `bridgedSucc_`.
  std::unordered_map<const NodeT *, NodeList> bridgedPred_;
};

/// View over the ICFG. Use `BridgeMode::Contract` to enable node contraction.
using FilteredICFGView = FilteredGraphView<ICFG, ICFGNode, ICFGEdge>;

/// Induced view over the SVFG. Bridging is compile-time disabled: contracting a
/// removed node would fabricate a value-flow path.
using FilteredSVFGView = FilteredGraphView<SVFG, SVFGNode, SVFGEdge>;

} // namespace lotus::analysis
