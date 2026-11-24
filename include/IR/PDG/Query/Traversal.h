/// Internal query implementation helpers; use the focused query headers in clients.
#pragma once

#include "IR/PDG/Query/QuerySupport.h"

namespace pdg::query_detail {

struct TraversalOutcome {
  NodeSet nodes;
  EdgeSet edges;
  std::unordered_map<Node *, std::set<Node *>> predecessors;
  std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
      predecessor_edges;
  std::unordered_map<Node *, size_t> distances;
};

TraversalOutcome traverseGraph(ProgramGraph &pdg, const NodeSet &start_nodes,
                               const std::set<EdgeType> &edge_types,
                               const PDGQueryOptions &options, bool forward,
                               PDGQueryDiagnostics &diagnostics);
PDGQueryResult resultFromTraversal(const NodeSet &criteria_nodes,
                                   const TraversalOutcome &outcome,
                                   PDGQueryDiagnostics diagnostics,
                                   bool explain);
} // namespace pdg::query_detail
