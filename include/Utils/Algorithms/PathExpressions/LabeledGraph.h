//===-- PathExpressions/LabeledGraph.h - Labeled graph interface ----------===//
//
// Directed labeled graph for path expression computation.
// Migrated from Ultimate Library-PathExpressions (v0.3.1).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus {
namespace pathexpressions {

/// Directed edge with label: source --(label)--> target.
template <typename N, typename L> struct ILabeledEdge {
  virtual ~ILabeledEdge() = default;
  virtual N getSource() const = 0;
  virtual N getTarget() const = 0;
  virtual L getLabel() const = 0;

  /// Structural equality & hashing, used for set semantics (like Java's
  /// HashSet).
  virtual std::size_t hashCode() const = 0;
  virtual bool equals(const ILabeledEdge<N, L> &other) const = 0;
};

namespace detail {
template <typename N, typename L> struct EdgePtrHash {
  std::size_t
  operator()(const std::shared_ptr<const ILabeledEdge<N, L>> &p) const {
    return p ? p->hashCode() : 0;
  }
};

template <typename N, typename L> struct EdgePtrEq {
  bool operator()(const std::shared_ptr<const ILabeledEdge<N, L>> &a,
                  const std::shared_ptr<const ILabeledEdge<N, L>> &b) const {
    if (a == b)
      return true;
    if (!a || !b)
      return false;
    return a->equals(*b);
  }
};
} // namespace detail

/// Directed labeled graph: nodes and edges with labels.
/// Faithfully mirrors Ultimate's Java interface: both nodes and edges are sets.
template <typename N, typename L> struct ILabeledGraph {
  using EdgePtr = std::shared_ptr<const ILabeledEdge<N, L>>;

  virtual ~ILabeledGraph() = default;
  virtual const std::unordered_set<N> &getNodes() const = 0;
  virtual const std::unordered_set<EdgePtr, detail::EdgePtrHash<N, L>,
                                   detail::EdgePtrEq<N, L>> &
  getEdges() const = 0;

  /// Optional deterministic insertion-ordered views (avoids pointer-hash
  /// non-determinism in path expression computation).
  virtual const std::vector<N> *getOrderedNodes() const { return nullptr; }
  virtual const std::vector<EdgePtr> *getOrderedEdges() const {
    return nullptr;
  }
};

/// Concrete labeled edge.
template <typename N, typename L>
class GenericLabeledEdge final : public ILabeledEdge<N, L> {
public:
  GenericLabeledEdge(N source, L label, N target)
      : source_(std::move(source)), label_(std::move(label)),
        target_(std::move(target)) {}

  N getSource() const override { return source_; }
  N getTarget() const override { return target_; }
  L getLabel() const override { return label_; }

  std::size_t hashCode() const override {
    std::size_t h = std::hash<N>()(source_);
    h ^= (std::hash<L>()(label_) << 1);
    h ^= (std::hash<N>()(target_) << 2);
    return h;
  }

  bool equals(const ILabeledEdge<N, L> &other) const override {
    const auto *o = dynamic_cast<const GenericLabeledEdge<N, L> *>(&other);
    return o != nullptr && source_ == o->source_ && label_ == o->label_ &&
           target_ == o->target_;
  }

private:
  N source_;
  L label_;
  N target_;
};

/// Concrete labeled graph with mutable nodes and edges.
template <typename N, typename L>
class GenericLabeledGraph : public ILabeledGraph<N, L> {
public:
  using EdgePtr = typename ILabeledGraph<N, L>::EdgePtr;

  const std::unordered_set<N> &getNodes() const override { return nodes_; }

  const std::unordered_set<EdgePtr, detail::EdgePtrHash<N, L>,
                           detail::EdgePtrEq<N, L>> &
  getEdges() const override {
    return edges_;
  }

  const std::vector<N> *getOrderedNodes() const override {
    return &orderedNodes_;
  }
  const std::vector<EdgePtr> *getOrderedEdges() const override {
    return &orderedEdges_;
  }

  bool addNode(N node) {
    if (nodes_.insert(node).second) {
      orderedNodes_.push_back(std::move(node));
      return true;
    }
    return false;
  }

  bool addEdge(std::shared_ptr<const ILabeledEdge<N, L>> edge) {
    if (!edge)
      return false;
    addNode(edge->getSource());
    addNode(edge->getTarget());
    if (edges_.insert(edge).second) {
      orderedEdges_.push_back(std::move(edge));
      return true;
    }
    return false;
  }

  bool addEdge(N source, L label, N target) {
    return addEdge(std::make_shared<GenericLabeledEdge<N, L>>(
        std::move(source), std::move(label), std::move(target)));
  }

private:
  std::unordered_set<N> nodes_;
  std::unordered_set<EdgePtr, detail::EdgePtrHash<N, L>,
                     detail::EdgePtrEq<N, L>>
      edges_;
  std::vector<N> orderedNodes_;
  std::vector<EdgePtr> orderedEdges_;
};

} // namespace pathexpressions
} // namespace lotus

