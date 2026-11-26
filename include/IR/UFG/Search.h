#pragma once


#include "IR/UFG/UFG.h"

namespace lotus {
namespace ufg {

/// One tabulation over a materialized object lane. Keys in foundAt are source
/// graph node IDs, so a caller can group reports across object lanes.
struct LaneSearchResult {
  usetracessa::QueryStatus status = usetracessa::QueryStatus::NotFound;
  usetracessa::SearchCompletion completion;
  std::map<FlowNodeID, QueryResult> foundAt;
  std::size_t productStates = 0;
  std::size_t productEdges = 0;
  std::size_t edgesExamined = 0;
  std::size_t summaryPairs = 0;
  bool exhaustive = true;
  std::string message;
};

class SearchEngine {
public:
  explicit SearchEngine(const UFGGraph &graph) : Graph(graph) {}
  LaneSearchResult scan(Query query, ObjectID object) const;
  QueryResult run(Query query, ObjectID object) const;
  LaneSearchResult scanGeneric(Query query) const;
  QueryResult runGeneric(Query query) const;

private:
  LaneSearchResult tabulate(Query query, const TraceFlowGraph &graph,
                            bool expanded) const;
  const UFGGraph &Graph;
};

} // namespace ufg
} // namespace lotus
