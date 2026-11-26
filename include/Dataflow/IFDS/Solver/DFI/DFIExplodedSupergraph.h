// Adapted from PhASAR's DFIExplodedSupergraph (88366fc69).
// Copyright (c) 2026 Fabian Schiebel and others. MIT; see
// third-party/phasar/LICENSE.txt.
#pragma once

#include "Utils/Algorithms/Graph/IntervalReachability.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ifds {
namespace dfi {
using Id = uint32_t;

/// ID-only DFI graph. Nodes are expanded once; new summary edges trigger local
/// interval-index reconstruction and re-discovery of entry-to-exit relations.
/// Ordinary returns are represented by matched caller summary edges, not by
/// unconstrained exit-to-caller edges in the query graph.
class ExplodedSupergraph {
public:
  struct Vertex {
    Id instruction, fact, function, local;
    bool exit;
  };
  struct Action {
    Id function;
    bool expand;
  };
  struct Summary {
    Id call, entry, exit;
  };
  struct Notifications {
    std::vector<Summary> summaries;
    std::vector<Id> unbalanced_exits;
  };
  explicit ExplodedSupergraph(
      std::shared_ptr<const lotus::graph::SCCGraph> sccs, bool follow_returns);
  Id vertex(Id instruction, Id fact, Id function, bool exit);
  const Vertex &get(Id id) const { return m_vertices.at(id); }
  size_t size() const { return m_vertices.size(); }
  void addSeed(Id vertex);
  void addEdge(Id from, Id to);
  bool addSummaryEdge(Id call, Id target);
  /// Registers a callee entry and its caller; returns previously known exits.
  std::vector<Id> addCallEdge(Id call, Id entry);
  void addUnbalancedReturn(Id exit, Id target);
  std::optional<Action> nextAction();
  std::vector<Id> takeWorklist(Id function);
  Notifications summarize(Id function);
  lotus::graph::Adjacency buildQueryGraph() const;
  size_t summaryEdgeCount() const { return m_summary_edges.size(); }

private:
  struct FunctionState {
    std::vector<Id> vertices, worklist;
    std::set<Id> roots, entries, seeds, unbalanced_roots, unbalanced_exits;
    std::vector<std::pair<Id, Id>> summaries;
    bool dirty = false;
  };
  void dirty(Id function);
  bool hasCallersForFact(Id function, Id fact) const;
  static uint64_t pair(Id a, Id b) { return (uint64_t(a) << 32) | b; }
  std::shared_ptr<const lotus::graph::SCCGraph> m_sccs;
  bool m_follow_returns;
  std::set<Id> m_pending;
  std::vector<FunctionState> m_functions;
  std::vector<Vertex> m_vertices;
  lotus::graph::Adjacency m_edges;
  std::unordered_map<uint64_t, Id> m_vertex_of;
  std::map<Id, std::set<Id>> m_callers, m_summary_exits;
  std::unordered_set<uint64_t> m_summary_edges, m_unbalanced_edges;
  std::vector<std::pair<Id, Id>> m_call_edges;
};
} // namespace dfi
} // namespace ifds
