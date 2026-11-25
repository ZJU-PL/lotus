#pragma once

#include "Alias/InclusionBased/BootstrapAA/Engine.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

// Dependency-free canonical Reduced Ordered Binary Decision Diagram (ROBDD)
// manager for path-sensitivity branching constraints (Kahlon PLDI '08, §3 p. 258).
//
// Soundness guarantee: if the node budget is exceeded, operations safely widen
// to trueNode() (never falseNode()), which can only weaken path constraints and
// never drop a feasible execution path.
class BddManager {
public:
  using NodeId = std::uint32_t;
  static constexpr NodeId FALSE_NODE = 0;
  static constexpr NodeId TRUE_NODE = 1;

  explicit BddManager(std::size_t max_nodes = 65536);

  NodeId falseNode() const { return FALSE_NODE; }
  NodeId trueNode() const { return TRUE_NODE; }
  bool isFalse(NodeId n) const { return n == FALSE_NODE; }
  bool isTrue(NodeId n) const { return n == TRUE_NODE; }

  // Literal for boolean condition variable `var`: polarity=true -> var,
  // polarity=false -> !var.
  NodeId literal(Id var, bool polarity);
  NodeId makeNot(NodeId a);
  NodeId makeAnd(NodeId a, NodeId b);
  NodeId makeOr(NodeId a, NodeId b);
  // Existential quantification ∃var. f = f[var=0] ∨ f[var=1].
  // Used to soundly forget loop-variant or reassigned condition variables.
  NodeId exists(NodeId a, Id var);
  // Existentially quantify a set of variables.
  NodeId existsAll(NodeId a, const std::vector<Id> &vars);

  std::size_t nodeCount() const { return m_nodes.size(); }
  bool budgetExceeded() const { return m_budget_exceeded; }

private:
  struct Node {
    Id var = INVALID;
    NodeId low = FALSE_NODE;
    NodeId high = FALSE_NODE;
  };
  struct TripleKey {
    Id var;
    NodeId low;
    NodeId high;
    bool operator==(const TripleKey &o) const {
      return var == o.var && low == o.low && high == o.high;
    }
  };
  struct TripleHash {
    std::size_t operator()(const TripleKey &k) const noexcept {
      std::size_t h = std::hash<std::uint64_t>{}(
          (static_cast<std::uint64_t>(k.var) << 32) ^ k.low);
      h ^= std::hash<std::uint32_t>{}(k.high) + 0x9e3779b97f4a7c15ULL +
           (h << 6) + (h >> 2);
      return h;
    }
  };
  struct OpKey {
    std::uint8_t op;
    NodeId a;
    NodeId b;
    bool operator==(const OpKey &o) const {
      return op == o.op && a == o.a && b == o.b;
    }
  };
  struct OpHash {
    std::size_t operator()(const OpKey &k) const noexcept {
      std::size_t h = std::hash<std::uint64_t>{}(
          (static_cast<std::uint64_t>(k.a) << 32) ^ k.b);
      h ^= static_cast<std::size_t>(k.op) * 0x9e3779b9ULL;
      return h;
    }
  };

  NodeId mk(Id var, NodeId low, NodeId high);
  NodeId applyAnd(NodeId a, NodeId b);
  NodeId applyOr(NodeId a, NodeId b);
  NodeId restrictVar(NodeId a, Id var, bool value);

  std::size_t m_max_nodes;
  bool m_budget_exceeded = false;
  std::vector<Node> m_nodes;
  std::unordered_map<TripleKey, NodeId, TripleHash> m_unique;
  std::unordered_map<OpKey, NodeId, OpHash> m_cache;
};

} // namespace detail
} // namespace bootstrap
} // namespace lotus
