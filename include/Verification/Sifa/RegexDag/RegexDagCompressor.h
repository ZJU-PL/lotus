//===-- Verification/Sifa/RegexDag/RegexDagCompressor.h -------------------===//
//
// RegexDag compression by node merging (ported from Ultimate Library-Sifa).
//
// Best-effort compression:
//  - not necessarily minimal,
//  - not canonical,
//  - but idempotent.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Verification/Sifa/RegexDag/RegexDag.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace sifa {

template <typename L> struct RegexRefHash {
  std::size_t operator()(const lotus::pathexpressions::RegexRef<L> &r) const {
    return r ? r->hashCode() : 0;
  }
};

template <typename L> struct RegexRefEq {
  bool operator()(const lotus::pathexpressions::RegexRef<L> &a,
                  const lotus::pathexpressions::RegexRef<L> &b) const {
    if (a == b)
      return true;
    if (!a || !b)
      return false;
    return a->equals(*b);
  }
};

template <typename L> class RegexDagCompressor final {
public:
  using Node = RegexDagNode<L>;
  using Dag = RegexDag<L>;
  using RegexRef = lotus::pathexpressions::RegexRef<L>;

  Dag &compress(Dag &dag) {
    dag_ = &dag;
    removed_.clear();
    mergedFlag_ = true;
    while (mergedFlag_) {
      mergedFlag_ = false;
      searchAndMerge(dag_->getSource(), /*forward=*/true);
      searchAndMerge(dag_->getSink(), /*forward=*/false);
    }
    return *dag_;
  }

private:
  void searchAndMerge(Node *startNode, bool forward) {
    if (!startNode || removed_.count(startNode)) {
      return;
    }
    std::unordered_set<Node *> visited;
    std::queue<Node *> work;
    visited.insert(startNode);
    work.push(startNode);

    while (!work.empty()) {
      Node *predator = work.front();
      work.pop();
      if (removed_.count(predator)) {
        continue;
      }

      mergeInDirection(predator, forward);
      if (removed_.count(predator)) {
        continue;
      }

      const auto &next =
          forward ? predator->getOutgoingNodes() : predator->getIncomingNodes();
      for (Node *n : next) {
        if (!removed_.count(n) && visited.insert(n).second) {
          work.push(n);
        }
      }
      eliminateIfEpsilon(predator);
    }
  }

  void mergeInDirection(Node *baseNode, bool forward) {
    mergeTable_.clear();
    const std::unordered_set<Node *> candidates =
        safeCandidates(baseNode, forward);
    if (candidates.size() <= 1) {
      return;
    }
    for (Node *c : candidates) {
      mergeTable_[c->getContent()].insert(c);
    }
    for (auto &kv : mergeTable_) {
      if (kv.second.size() > 1) {
        groupToSingleNode(kv.first, kv.second);
      }
    }
  }

  std::unordered_set<Node *> safeCandidates(Node *base, bool forward) {
    std::unordered_set<Node *> candidates;
    const auto &direct =
        forward ? base->getOutgoingNodes() : base->getIncomingNodes();
    if (direct.size() <= 1) {
      return candidates;
    }

    // Fast path: if all direct non-removed neighbors have distinct labels,
    // no merge group of size >= 2 can exist, so skip reachability BFS.
    std::unordered_set<RegexRef, RegexRefHash<L>, RegexRefEq<L>> seenLabels;
    bool hasDuplicateLabel = false;
    for (Node *n : direct) {
      if (!n || removed_.count(n)) {
        continue;
      }
      candidates.insert(n);
      if (!seenLabels.insert(n->getContent()).second) {
        hasDuplicateLabel = true;
      }
    }
    if (!hasDuplicateLabel || candidates.size() <= 1) {
      return {};
    }

    // Remove candidates that are transitively reachable from other candidates
    // using a single multi-source BFS starting from direct neighbors' edges.
    std::unordered_set<Node *> transitivelyReachable;
    std::queue<Node *> q;
    std::unordered_set<Node *> seen;
    for (Node *start : candidates) {
      const auto &nbrs =
          forward ? start->getOutgoingNodes() : start->getIncomingNodes();
      for (Node *n : nbrs) {
        if (!n || removed_.count(n)) {
          continue;
        }
        if (candidates.count(n)) {
          transitivelyReachable.insert(n);
        }
        if (seen.insert(n).second) {
          q.push(n);
        }
      }
    }
    while (!q.empty()) {
      Node *cur = q.front();
      q.pop();
      const auto &nbrs =
          forward ? cur->getOutgoingNodes() : cur->getIncomingNodes();
      for (Node *n : nbrs) {
        if (!n || removed_.count(n)) {
          continue;
        }
        if (candidates.count(n)) {
          transitivelyReachable.insert(n);
        }
        if (seen.insert(n).second) {
          q.push(n);
        }
      }
    }

    for (Node *n : transitivelyReachable) {
      candidates.erase(n);
    }
    return candidates;
  }

  Node *groupToSingleNode(const RegexRef &label,
                          const std::unordered_set<Node *> &mergeGroup) {
    if (mergeGroup.size() <= 1) {
      return *mergeGroup.begin();
    }
    mergedFlag_ = true;
    Node *merged = dag_->makeNode(label);

    std::unordered_set<Node *> seenIn;
    std::unordered_set<Node *> seenOut;
    std::vector<Node *> touchedInNeighbors;
    std::vector<Node *> touchedOutNeighbors;

    for (Node *prey : mergeGroup) {
      if (!prey || prey == merged || removed_.count(prey))
        continue;
      for (Node *in : prey->getIncomingNodes()) {
        if (in && !removed_.count(in) && !mergeGroup.count(in) &&
            seenIn.insert(in).second) {
          touchedInNeighbors.push_back(in);
        }
      }
      for (Node *out : prey->getOutgoingNodes()) {
        if (out && !removed_.count(out) && !mergeGroup.count(out) &&
            seenOut.insert(out).second) {
          touchedOutNeighbors.push_back(out);
        }
      }
    }

    // Batch-strip all merged prey nodes from each touched neighbor in one scan,
    // then connect `merged` in O(1) per unique neighbor (`merged` is brand-new).
    for (Node *in : touchedInNeighbors) {
      in->removeOutgoingIf(
          [&](const Node *n) { return mergeGroup.count(const_cast<Node *>(n)) != 0; });
      merged->connectIncomingUnique(in);
    }
    for (Node *out : touchedOutNeighbors) {
      out->removeIncomingIf(
          [&](const Node *n) { return mergeGroup.count(const_cast<Node *>(n)) != 0; });
      merged->connectOutgoingUnique(out);
    }

    for (Node *prey : mergeGroup) {
      if (!prey || prey == merged)
        continue;
      if (prey == dag_->getSink()) {
        dag_->setSink(merged);
      }
      if (prey == dag_->getSource()) {
        dag_->setSource(merged);
      }
      prey->clearEdges();
      removed_.insert(prey);
    }
    return merged;
  }

  void merge(Node *predator, Node *prey) {
    if (!predator || !prey || predator == prey || removed_.count(prey)) {
      return;
    }
    mergedFlag_ = true;

    // Remove prey from its neighbors.
    for (Node *in : prey->getIncomingNodes()) {
      in->removeOutgoing(prey);
    }
    for (Node *out : prey->getOutgoingNodes()) {
      out->removeIncoming(prey);
    }

    // Copy incoming edges to predator.
    std::unordered_set<Node *> ignore(predator->getIncomingNodes().begin(),
                                      predator->getIncomingNodes().end());
    ignore.insert(predator);
    for (Node *in : prey->getIncomingNodes()) {
      if (!removed_.count(in) && !ignore.count(in)) {
        predator->connectIncoming(in);
      }
    }

    // Copy outgoing edges to predator.
    ignore.clear();
    ignore.insert(predator->getOutgoingNodes().begin(),
                  predator->getOutgoingNodes().end());
    ignore.insert(predator);
    for (Node *out : prey->getOutgoingNodes()) {
      if (!removed_.count(out) && !ignore.count(out)) {
        predator->connectOutgoing(out);
      }
    }

    if (prey == dag_->getSink()) {
      dag_->setSink(predator);
    }
    if (prey == dag_->getSource()) {
      dag_->setSource(predator);
    }

    prey->clearEdges();
    removed_.insert(prey);
  }

  void eliminateIfEpsilon(Node *eps) {
    if (!eps || removed_.count(eps) || !eps->isEpsilon()) {
      return;
    }

    const auto &in0 = eps->getIncomingNodes();
    const auto &out0 = eps->getOutgoingNodes();
    if (in0.empty() && out0.empty()) {
      return;
    }
    // Avoid quadratic bipartite edge blowup when eliminating a high-degree join
    // epsilon node (compression is best-effort).
    if (in0.size() > 1 && out0.size() > 1 &&
        in0.size() * out0.size() > in0.size() + out0.size()) {
      return;
    }

    if (eps == dag_->getSource()) {
      if (out0.size() != 1) {
        return;
      }
      dag_->setSource(out0.front());
    } else if (eps == dag_->getSink()) {
      if (in0.size() != 1) {
        return;
      }
      dag_->setSink(in0.front());
    }

    std::vector<Node *> in(in0.begin(), in0.end());
    std::vector<Node *> out(out0.begin(), out0.end());
    for (Node *inNode : in) {
      for (Node *outNode : out) {
        inNode->connectOutgoing(outNode);
      }
    }
    for (Node *inNode : in) {
      inNode->removeOutgoing(eps);
    }
    for (Node *outNode : out) {
      outNode->removeIncoming(eps);
    }
    eps->clearEdges();
    removed_.insert(eps);
    mergedFlag_ = true;
  }

  Dag *dag_ = nullptr;
  bool mergedFlag_ = false;
  std::unordered_set<Node *> removed_;

  std::unordered_map<RegexRef, std::unordered_set<Node *>, RegexRefHash<L>,
                     RegexRefEq<L>>
      mergeTable_;
};

} // namespace sifa
} // namespace lotus

