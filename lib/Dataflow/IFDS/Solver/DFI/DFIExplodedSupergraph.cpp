// Adapted from PhASAR's DFIExplodedSupergraph (88366fc69).
// Copyright (c) 2026 Fabian Schiebel and others. MIT; see
// third-party/phasar/LICENSE.txt.
#include "Dataflow/IFDS/Solver/DFI/DFIExplodedSupergraph.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ifds {
namespace dfi {
ExplodedSupergraph::ExplodedSupergraph(
    std::shared_ptr<const lotus::graph::SCCGraph> sccs, bool follow_returns)
    : m_sccs(std::move(sccs)), m_follow_returns(follow_returns) {
  if (!m_sccs)
    throw std::invalid_argument("DFI needs a call graph SCC decomposition");
  m_functions.resize(m_sccs->numVertices());
}
void ExplodedSupergraph::dirty(Id function) {
  m_functions.at(function).dirty = true;
  m_pending.insert(m_sccs->componentOf(function));
}
Id ExplodedSupergraph::vertex(Id instruction, Id fact, Id function, bool exit) {
  auto found = m_vertex_of.find(pair(instruction, fact));
  if (found != m_vertex_of.end())
    return found->second;
  if (size() >= std::numeric_limits<Id>::max() - 2)
    throw std::length_error("Too many DFI vertices");
  auto &state = m_functions.at(function);
  auto id = static_cast<Id>(size());
  m_vertex_of.emplace(pair(instruction, fact), id);
  m_vertices.push_back({instruction, fact, function,
                        static_cast<Id>(state.vertices.size()), exit});
  m_edges.emplace_back();
  state.vertices.push_back(id);
  state.worklist.push_back(id);
  dirty(function);
  return id;
}
void ExplodedSupergraph::addSeed(Id v) {
  auto &state = m_functions[get(v).function];
  state.seeds.insert(v);
  state.roots.insert(v);
  dirty(get(v).function);
}
void ExplodedSupergraph::addEdge(Id from, Id to) {
  if (get(from).function != get(to).function)
    throw std::logic_error("DFI normal edges must stay within a function");
  m_edges.at(from).push_back(to);
}
bool ExplodedSupergraph::addSummaryEdge(Id call, Id target) {
  if (get(call).function != get(target).function)
    throw std::logic_error("DFI summary endpoints must share a function");
  if (!m_summary_edges.insert(pair(call, target)).second)
    return false;
  auto fun = get(call).function;
  m_functions[fun].summaries.emplace_back(call, target);
  dirty(fun);
  return true;
}
std::vector<Id> ExplodedSupergraph::addCallEdge(Id call, Id entry) {
  auto fun = get(entry).function;
  auto &state = m_functions[fun];
  state.entries.insert(entry);
  state.roots.insert(entry);
  if (m_callers[entry].insert(call).second) {
    m_call_edges.emplace_back(call, entry);
    dirty(fun);
  }
  const auto &known = m_summary_exits[entry];
  return {known.begin(), known.end()};
}
void ExplodedSupergraph::addUnbalancedReturn(Id exit, Id target) {
  if (!m_unbalanced_edges.insert(pair(exit, target)).second)
    return;
  auto fun = get(target).function;
  auto &state = m_functions[fun];
  state.roots.insert(target);
  state.unbalanced_roots.insert(target);
  dirty(fun);
}
std::optional<ExplodedSupergraph::Action> ExplodedSupergraph::nextAction() {
  while (!m_pending.empty()) {
    auto component = *m_pending.begin();
    for (auto fun : m_sccs->members(component))
      if (!m_functions[fun].worklist.empty())
        return Action{fun, true};
    for (auto fun : m_sccs->members(component))
      if (m_functions[fun].dirty)
        return Action{fun, false};
    m_pending.erase(component);
  }
  return std::nullopt;
}
std::vector<Id> ExplodedSupergraph::takeWorklist(Id function) {
  std::vector<Id> result;
  result.swap(m_functions.at(function).worklist);
  std::sort(result.begin(), result.end(), [this](Id a, Id b) {
    return get(a).instruction < get(b).instruction;
  });
  return result;
}
bool ExplodedSupergraph::hasCallersForFact(Id fun, Id fact) const {
  for (auto entry : m_functions[fun].entries) {
    auto found = m_callers.find(entry);
    if (get(entry).fact == fact && found != m_callers.end() &&
        !found->second.empty())
      return true;
  }
  return false;
}
ExplodedSupergraph::Notifications ExplodedSupergraph::summarize(Id function) {
  auto &state = m_functions.at(function);
  state.dirty = false;
  lotus::graph::Adjacency local(state.vertices.size());
  std::vector<bool> exits(local.size(), false);
  std::vector<Id> roots;
  for (auto v : state.vertices) {
    exits[get(v).local] = get(v).exit;
    for (auto to : m_edges[v])
      local[get(v).local].push_back(get(to).local);
  }
  for (auto edge : state.summaries)
    local[get(edge.first).local].push_back(get(edge.second).local);
  for (auto root : state.roots)
    roots.push_back(get(root).local);
  lotus::graph::IntervalReachabilityIndex index(local, roots, exits);
  Notifications result;
  auto collect = [&](Id root, std::set<Id> &into) {
    index.forEachReachableTarget(get(root).local, [&](Id local_exit) {
      into.insert(state.vertices[local_exit]);
    });
  };
  for (auto entry : state.entries) {
    std::set<Id> reachable;
    collect(entry, reachable);
    // Unbalanced returns enter the zero context, matching the IFDS convention.
    if (get(entry).fact == 0)
      for (auto root : state.unbalanced_roots)
        collect(root, reachable);
    auto &known = m_summary_exits[entry];
    for (auto exit : reachable)
      if (known.insert(exit).second)
        for (auto caller : m_callers[entry])
          result.summaries.push_back({caller, entry, exit});
  }
  if (m_follow_returns) {
    std::set<Id> reachable;
    for (auto seed : state.seeds)
      if (!hasCallersForFact(function, get(seed).fact))
        collect(seed, reachable);
    if (!hasCallersForFact(function, 0))
      for (auto root : state.unbalanced_roots)
        collect(root, reachable);
    for (auto exit : reachable)
      if (state.unbalanced_exits.insert(exit).second)
        result.unbalanced_exits.push_back(exit);
  }
  // Notifications own their IDs; callbacks can safely grow the graph.
  return result;
}
lotus::graph::Adjacency ExplodedSupergraph::buildQueryGraph() const {
  auto result = m_edges;
  for (const auto &state : m_functions)
    for (auto edge : state.summaries)
      result[edge.first].push_back(edge.second);
  for (auto edge : m_call_edges)
    result[edge.first].push_back(edge.second);
  for (auto edge : m_unbalanced_edges)
    result[static_cast<Id>(edge >> 32)].push_back(static_cast<Id>(edge));
  return result;
}
} // namespace dfi
} // namespace ifds
