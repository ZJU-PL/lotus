#include "Alias/InclusionBased/BootstrapAA/Support/Bdd.h"

#include <algorithm>

namespace lotus {
namespace bootstrap {
namespace detail {

namespace {
constexpr std::uint8_t OP_AND = 1;
constexpr std::uint8_t OP_OR = 2;
constexpr std::uint8_t OP_NOT = 3;
constexpr std::uint8_t OP_EXISTS = 4;
constexpr std::uint8_t OP_RESTRICT_0 = 5;
constexpr std::uint8_t OP_RESTRICT_1 = 6;
} // namespace

BddManager::BddManager(std::size_t max_nodes)
    : m_max_nodes(std::max<std::size_t>(max_nodes, 4)) {
  m_nodes.push_back({INVALID, FALSE_NODE, FALSE_NODE}); // 0 = FALSE
  m_nodes.push_back({INVALID, TRUE_NODE, TRUE_NODE});   // 1 = TRUE
}

BddManager::NodeId BddManager::mk(Id var, NodeId low, NodeId high) {
  if (low == high)
    return low;
  TripleKey key{var, low, high};
  auto it = m_unique.find(key);
  if (it != m_unique.end())
    return it->second;
  if (m_nodes.size() >= m_max_nodes) {
    m_budget_exceeded = true;
    return TRUE_NODE;
  }
  const NodeId id = static_cast<NodeId>(m_nodes.size());
  m_nodes.push_back({var, low, high});
  m_unique.emplace(key, id);
  return id;
}

BddManager::NodeId BddManager::literal(Id var, bool polarity) {
  if (var == INVALID)
    return TRUE_NODE;
  return polarity ? mk(var, FALSE_NODE, TRUE_NODE)
                  : mk(var, TRUE_NODE, FALSE_NODE);
}

BddManager::NodeId BddManager::makeNot(NodeId a) {
  if (m_budget_exceeded)
    return TRUE_NODE;
  if (a == FALSE_NODE)
    return TRUE_NODE;
  if (a == TRUE_NODE)
    return FALSE_NODE;
  OpKey key{OP_NOT, a, 0};
  auto it = m_cache.find(key);
  if (it != m_cache.end())
    return it->second;
  const Node n = m_nodes[a];
  const NodeId lo = makeNot(n.low);
  const NodeId hi = makeNot(n.high);
  if (m_budget_exceeded)
    return TRUE_NODE;
  const NodeId res = mk(n.var, lo, hi);
  if (m_budget_exceeded)
    return TRUE_NODE;
  m_cache.emplace(key, res);
  return res;
}

BddManager::NodeId BddManager::makeAnd(NodeId a, NodeId b) {
  if (m_budget_exceeded)
    return TRUE_NODE;
  NodeId r = applyAnd(a, b);
  return m_budget_exceeded ? TRUE_NODE : r;
}

BddManager::NodeId BddManager::makeOr(NodeId a, NodeId b) {
  if (m_budget_exceeded)
    return TRUE_NODE;
  NodeId r = applyOr(a, b);
  return m_budget_exceeded ? TRUE_NODE : r;
}

BddManager::NodeId BddManager::applyAnd(NodeId a, NodeId b) {
  if (m_budget_exceeded)
    return TRUE_NODE;
  if (a == FALSE_NODE || b == FALSE_NODE)
    return FALSE_NODE;
  if (a == TRUE_NODE)
    return b;
  if (b == TRUE_NODE)
    return a;
  if (a == b)
    return a;
  if (a > b)
    std::swap(a, b);
  OpKey key{OP_AND, a, b};
  auto it = m_cache.find(key);
  if (it != m_cache.end())
    return it->second;
  const Node na = m_nodes[a];
  const Node nb = m_nodes[b];
  NodeId res = TRUE_NODE;
  if (na.var == nb.var) {
    const NodeId lo = applyAnd(na.low, nb.low);
    const NodeId hi = applyAnd(na.high, nb.high);
    res = mk(na.var, lo, hi);
  } else if (na.var < nb.var) {
    const NodeId lo = applyAnd(na.low, b);
    const NodeId hi = applyAnd(na.high, b);
    res = mk(na.var, lo, hi);
  } else {
    const NodeId lo = applyAnd(a, nb.low);
    const NodeId hi = applyAnd(a, nb.high);
    res = mk(nb.var, lo, hi);
  }
  if (!m_budget_exceeded)
    m_cache.emplace(key, res);
  return res;
}

BddManager::NodeId BddManager::applyOr(NodeId a, NodeId b) {
  if (m_budget_exceeded)
    return TRUE_NODE;
  if (a == TRUE_NODE || b == TRUE_NODE)
    return TRUE_NODE;
  if (a == FALSE_NODE)
    return b;
  if (b == FALSE_NODE)
    return a;
  if (a == b)
    return a;
  if (a > b)
    std::swap(a, b);
  OpKey key{OP_OR, a, b};
  auto it = m_cache.find(key);
  if (it != m_cache.end())
    return it->second;
  const Node na = m_nodes[a];
  const Node nb = m_nodes[b];
  NodeId res = TRUE_NODE;
  if (na.var == nb.var) {
    const NodeId lo = applyOr(na.low, nb.low);
    const NodeId hi = applyOr(na.high, nb.high);
    res = mk(na.var, lo, hi);
  } else if (na.var < nb.var) {
    const NodeId lo = applyOr(na.low, b);
    const NodeId hi = applyOr(na.high, b);
    res = mk(na.var, lo, hi);
  } else {
    const NodeId lo = applyOr(a, nb.low);
    const NodeId hi = applyOr(a, nb.high);
    res = mk(nb.var, lo, hi);
  }
  if (!m_budget_exceeded)
    m_cache.emplace(key, res);
  return res;
}

BddManager::NodeId BddManager::restrictVar(NodeId a, Id var, bool value) {
  if (a == FALSE_NODE || a == TRUE_NODE)
    return a;
  if (m_budget_exceeded)
    return TRUE_NODE;
  const Node n = m_nodes[a];
  if (n.var > var)
    return a;
  if (n.var == var)
    return value ? n.high : n.low;
  OpKey key{value ? OP_RESTRICT_1 : OP_RESTRICT_0, a, var};
  auto it = m_cache.find(key);
  if (it != m_cache.end())
    return it->second;
  const NodeId lo = restrictVar(n.low, var, value);
  const NodeId hi = restrictVar(n.high, var, value);
  if (m_budget_exceeded)
    return TRUE_NODE;
  const NodeId res = mk(n.var, lo, hi);
  if (!m_budget_exceeded)
    m_cache.emplace(key, res);
  return m_budget_exceeded ? TRUE_NODE : res;
}

BddManager::NodeId BddManager::exists(NodeId a, Id var) {
  if (a == FALSE_NODE || a == TRUE_NODE || var == INVALID)
    return a;
  if (m_budget_exceeded)
    return TRUE_NODE;
  const Node n = m_nodes[a];
  if (n.var > var)
    return a;
  OpKey key{OP_EXISTS, a, var};
  auto it = m_cache.find(key);
  if (it != m_cache.end())
    return it->second;
  NodeId res = TRUE_NODE;
  if (n.var == var) {
    res = applyOr(n.low, n.high);
  } else {
    const NodeId lo = exists(n.low, var);
    const NodeId hi = exists(n.high, var);
    res = mk(n.var, lo, hi);
  }
  if (m_budget_exceeded)
    return TRUE_NODE;
  m_cache.emplace(key, res);
  return res;
}

BddManager::NodeId BddManager::existsAll(NodeId a,
                                         const std::vector<Id> &vars) {
  for (Id v : vars) {
    a = exists(a, v);
    if (a == TRUE_NODE || a == FALSE_NODE)
      break;
  }
  return a;
}

BddManager::NodeId BddManager::substitute(NodeId f, Id var,
                                          NodeId replacement) {
  if (f == FALSE_NODE || f == TRUE_NODE || var == INVALID)
    return f;
  if (m_budget_exceeded)
    return TRUE_NODE;
  if (m_nodes[f].var > var)
    return f;
  if (replacement == TRUE_NODE) {
    const NodeId r = restrictVar(f, var, true);
    return m_budget_exceeded ? TRUE_NODE : r;
  }
  if (replacement == FALSE_NODE) {
    const NodeId r = restrictVar(f, var, false);
    return m_budget_exceeded ? TRUE_NODE : r;
  }
  const NodeId f1 = restrictVar(f, var, true);
  const NodeId f0 = restrictVar(f, var, false);
  if (m_budget_exceeded)
    return TRUE_NODE;
  if (f1 == f0)
    return f1;
  const NodeId neg_rep = makeNot(replacement);
  const NodeId t_branch = makeAnd(replacement, f1);
  const NodeId f_branch = makeAnd(neg_rep, f0);
  return makeOr(t_branch, f_branch);
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
