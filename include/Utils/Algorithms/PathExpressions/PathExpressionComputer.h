//===-- PathExpressions/PathExpressionComputer.h - Tarjan path expressions ===//
//
// Computes path expressions for a labeled graph (regular expression describing
// all paths between two nodes). Algorithm: Tarjan, "Fast Algorithms for
// Solving Path Problems", 1981, Chapter 2.
//
// Migrated from Ultimate Library-PathExpressions (v0.3.1).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Utils/Algorithms/PathExpressions/LabeledGraph.h"
#include "Utils/Algorithms/PathExpressions/Regex.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus {
namespace pathexpressions {

namespace detail {
struct IntPairHash {
  std::size_t operator()(const std::pair<int, int> &p) const {
    return std::hash<int>()(p.first) ^ (std::hash<int>()(p.second) << 1);
  }
};
} // namespace detail

/// Computes path expressions for a labeled graph.
/// For a fixed source node, returns a regex describing all paths to each node.
/// Complexity: O(n^3 + m) for n nodes, m edges (Tarjan Chapter 2).
template <typename N, typename L> class PathExpressionComputer {
public:
  using RegexRefT = RegexRef<L>;
  using Graph = ILabeledGraph<N, L>;

  explicit PathExpressionComputer(const Graph &graph) : graph_(graph) {
    mapNodesToInt();
  }

  /// Path expression (regex) from \p source to \p target.
  /// Nodes must be in the graph.
  RegexRefT exprBetween(const N &source, const N &target) {
    assert(graph_.getNodes().count(source) &&
           "Tried to compute path expression starting at non-existing node");
    assert(graph_.getNodes().count(target) &&
           "Tried to compute path expression ending at non-existing node");

    auto it = allPathsFromNode_.find(source);
    if (it == allPathsFromNode_.end()) {
      eliminate();
      auto allPathsFromSource = solve(source, extractPathSequence());
      it = allPathsFromNode_.emplace(source, std::move(allPathsFromSource))
               .first;
    }
    return it->second.at(static_cast<std::size_t>(intOf(target)));
  }

private:
  struct PathExpression {
    RegexRefT expr;
    int source;
    int target;
  };

  const Graph &graph_;
  std::unordered_map<N, int> nodeToInt_;
  std::vector<N> intToNode_;
  std::unordered_map<std::pair<int, int>, RegexRefT, detail::IntPairHash> P_;
  std::unordered_map<N, std::vector<RegexRefT>> allPathsFromNode_;
  bool eliminated_ = false;

  void mapNodesToInt() {
    std::vector<N> nodes;
    if (const auto *ordered = graph_.getOrderedNodes()) {
      nodes = *ordered;
    } else {
      const auto &rawNodes = graph_.getNodes();
      nodes.assign(rawNodes.begin(), rawNodes.end());
      if constexpr (!std::is_pointer<N>::value) {
        std::sort(nodes.begin(), nodes.end());
      }
    }

    const std::size_t n = nodes.size();
    if (n == 0) {
      return;
    }

    std::unordered_map<N, std::size_t> rawIdx;
    rawIdx.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      rawIdx.emplace(nodes[i], i);
    }

    // Build a deterministic min-fill (minimum inDeg * outDeg) elimination order,
    // keeping entry (inDeg == 0) and exit (outDeg == 0) nodes last so interior
    // diamonds/loops contract locally before reaching source/sink.
    std::vector<std::vector<std::size_t>> succs(n), preds(n);
    auto addAdj = [&](const auto &edge) {
      auto itS = rawIdx.find(edge->getSource());
      auto itT = rawIdx.find(edge->getTarget());
      if (itS == rawIdx.end() || itT == rawIdx.end()) {
        return;
      }
      const std::size_t u = itS->second;
      const std::size_t v = itT->second;
      if (u == v) {
        return;
      }
      succs[u].push_back(v);
      preds[v].push_back(u);
    };
    if (const auto *orderedEdges = graph_.getOrderedEdges()) {
      for (const auto &edge : *orderedEdges) {
        addAdj(edge);
      }
    } else {
      for (const auto &edge : graph_.getEdges()) {
        addAdj(edge);
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      std::sort(succs[i].begin(), succs[i].end());
      succs[i].erase(std::unique(succs[i].begin(), succs[i].end()),
                     succs[i].end());
      std::sort(preds[i].begin(), preds[i].end());
      preds[i].erase(std::unique(preds[i].begin(), preds[i].end()),
                     preds[i].end());
    }

    std::vector<std::size_t> order;
    order.reserve(n);
    if (n <= 1024) {
      std::vector<bool> eliminated(n, false);
      std::vector<std::size_t> inDeg(n, 0), outDeg(n, 0);
      for (std::size_t i = 0; i < n; ++i) {
        inDeg[i] = preds[i].size();
        outDeg[i] = succs[i].size();
      }
      // Dense adjacency bitset/byte table for fast min-fill simulation on
      // medium-sized CFGs.
      std::vector<uint8_t> hasEdge(n * n, 0);
      for (std::size_t u = 0; u < n; ++u) {
        for (std::size_t v : succs[u]) {
          hasEdge[u * n + v] = 1;
        }
      }
      for (std::size_t step = 0; step < n; ++step) {
        std::size_t best = n;
        std::size_t bestTier = 3;
        std::size_t bestScore = std::numeric_limits<std::size_t>::max();
        for (std::size_t v = 0; v < n; ++v) {
          if (eliminated[v]) {
            continue;
          }
          // Tier 0: interior nodes (in > 0 && out > 0)
          // Tier 1: pure exits (out == 0, in > 0)
          // Tier 2: pure entries / isolated (in == 0)
          const std::size_t tier =
              (inDeg[v] == 0) ? 2 : ((outDeg[v] == 0) ? 1 : 0);
          const std::size_t score = inDeg[v] * outDeg[v];
          if (tier < bestTier || (tier == bestTier && score < bestScore)) {
            best = v;
            bestTier = tier;
            bestScore = score;
            if (bestTier == 0 && bestScore <= 1) {
              break;
            }
          }
        }
        eliminated[best] = true;
        order.push_back(best);
        // Simulate eliminating `best` for remaining vertices.
        std::vector<std::size_t> livePreds, liveSuccs;
        for (std::size_t u : preds[best]) {
          if (!eliminated[u]) {
            livePreds.push_back(u);
          }
        }
        for (std::size_t w : succs[best]) {
          if (!eliminated[w]) {
            liveSuccs.push_back(w);
          }
        }
        for (std::size_t u : livePreds) {
          if (outDeg[u] > 0) {
            --outDeg[u];
          }
        }
        for (std::size_t w : liveSuccs) {
          if (inDeg[w] > 0) {
            --inDeg[w];
          }
        }
        for (std::size_t u : livePreds) {
          for (std::size_t w : liveSuccs) {
            if (u == w) {
              continue;
            }
            if (!hasEdge[u * n + w]) {
              hasEdge[u * n + w] = 1;
              succs[u].push_back(w);
              preds[w].push_back(u);
              ++outDeg[u];
              ++inDeg[w];
            }
          }
        }
      }
    } else {
      for (std::size_t i = 0; i < n; ++i) {
        order.push_back(i);
      }
    }

    intToNode_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      const N &node = nodes[order[i]];
      nodeToInt_[node] = static_cast<int>(i);
      intToNode_.push_back(node);
    }
  }

  int intOf(const N &node) const {
    auto it = nodeToInt_.find(node);
    if (it == nodeToInt_.end()) {
      throw std::invalid_argument(
          "Tried to access node which is not in the graph");
    }
    return it->second;
  }

  RegexRefT pathExpr(const int source, const int target) const {
    const auto it = P_.find({source, target});
    if (it == P_.end()) {
      return Regex<L>::emptySet();
    }
    return it->second;
  }

  void updatePathExpr(const int source, const int target, RegexRefT newExpr) {
    P_[{source, target}] = std::move(newExpr);
  }

  /// Tarjan's elimination phase: fill P(u,v) for all u,v.
  void eliminate() {
    if (eliminated_)
      return;

    const int n = static_cast<int>(intToNode_.size());
    auto insertEdge = [&](const auto &edge) {
      const int head = intOf(edge->getSource());
      const int tail = intOf(edge->getTarget());
      auto pht = pathExpr(head, tail);
      pht = Regex<L>::simplifiedUnion(Regex<L>::literal(edge->getLabel()), pht);
      updatePathExpr(head, tail, std::move(pht));
    };
    if (const auto *orderedEdges = graph_.getOrderedEdges()) {
      for (const auto &edge : *orderedEdges) {
        insertEdge(edge);
      }
    } else {
      for (const auto &edge : graph_.getEdges()) {
        insertEdge(edge);
      }
    }

    for (int v = 0; v < n; v++) {
      auto pvv = pathExpr(v, v);
      pvv = Regex<L>::simplifiedStar(pvv);
      updatePathExpr(v, v, pvv);
      for (int u = v + 1; u < n; u++) {
        auto puv = pathExpr(u, v);
        if (puv->isEmptySet()) {
          continue;
        }
        puv = Regex<L>::simplifiedConcatenation(puv, pvv);
        updatePathExpr(u, v, puv);
        for (int w = v + 1; w < n; w++) {
          const auto pvw = pathExpr(v, w);
          if (pvw->isEmptySet()) {
            continue;
          }
          const auto oldPuw = pathExpr(u, w);
          const auto a = Regex<L>::simplifiedConcatenation(puv, pvw);
          const auto puw = Regex<L>::simplifiedUnion(oldPuw, a);
          updatePathExpr(u, w, puw);
        }
      }
    }
    eliminated_ = true;
  }

  std::vector<PathExpression> extractPathSequence() const {
    const int n = static_cast<int>(graph_.getNodes().size());
    std::vector<PathExpression> pathSequence;
    for (int u = 0; u < n; u++) {
      for (int w = u; w < n; w++) {
        const auto reg = pathExpr(u, w);
        if (!reg->isEmptySet() && !reg->isEpsilon()) {
          pathSequence.push_back({reg, u, w});
        }
      }
    }
    for (int u = n - 1; u >= 0; u--) {
      for (int w = 0; w < u; w++) {
        const auto reg = pathExpr(u, w);
        if (!reg->isEmptySet()) {
          pathSequence.push_back({reg, u, w});
        }
      }
    }
    return pathSequence;
  }

  std::vector<RegexRefT>
  solve(const N &source,
        const std::vector<PathExpression> &pathSequence) const {
    const std::size_t n = graph_.getNodes().size();
    std::vector<RegexRefT> allPathsFromSource(n, Regex<L>::emptySet());
    allPathsFromSource.at(static_cast<std::size_t>(intOf(source))) =
        Regex<L>::epsilon();

    for (const auto &seqElement : pathSequence) {
      if (seqElement.source == seqElement.target) {
        const int vi = seqElement.source;
        const auto regexVi =
            allPathsFromSource.at(static_cast<std::size_t>(vi));
        allPathsFromSource.at(static_cast<std::size_t>(vi)) =
            Regex<L>::simplifiedConcatenation(regexVi, seqElement.expr);
      } else {
        const int vi = seqElement.source;
        const int wi = seqElement.target;
        const auto regexVi =
            allPathsFromSource.at(static_cast<std::size_t>(vi));
        const auto inter =
            Regex<L>::simplifiedConcatenation(regexVi, seqElement.expr);
        const auto regexWi =
            allPathsFromSource.at(static_cast<std::size_t>(wi));
        allPathsFromSource.at(static_cast<std::size_t>(wi)) =
            Regex<L>::simplifiedUnion(regexWi, inter);
      }
    }
    return allPathsFromSource;
  }
};

} // namespace pathexpressions
} // namespace lotus

