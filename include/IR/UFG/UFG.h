#pragma once

#include "IR/UseTraceSSA/Query.h"
#include "IR/UFG/ID.h"

namespace lotus {
namespace ufg {

using usetracessa::FlowNodeID;
using usetracessa::ObjectID;
using usetracessa::Query;
using usetracessa::QueryResult;
using usetracessa::TraceFlowGraph;

struct Statistics {
  std::size_t objects = 0;
  std::size_t nodes = 0;
  std::size_t edges = 0;
};

/// Explicit object-expanded use-flow graph. Each lane contains its own copy of
/// the temporal topology; guards are resolved during construction. The source
/// graph must outlive this graph so generic (non-resource) clients can use it.
class UFGGraph {
public:
  explicit UFGGraph(const TraceFlowGraph &source,
                    std::vector<ObjectID> universe = {});

  const TraceFlowGraph &graph() const { return Expanded; }
  const TraceFlowGraph &source() const { return Source; }
  const std::vector<ObjectID> &objects() const { return Objects; }
  Statistics statistics() const;
  FlowNodeID node(ObjectID object, FlowNodeID original) const;
  FlowNodeID originalNode(FlowNodeID expanded) const;
  ObjectID objectOf(FlowNodeID expanded) const;
  QueryResult runObject(Query query, ObjectID object) const;
  void printDOT(std::ostream &out) const;
  void printJSON(std::ostream &out) const;

private:
  const TraceFlowGraph &Source;
  TraceFlowGraph Expanded;
  std::vector<ObjectID> Objects;
  std::vector<FlowNodeID> OriginalNodes;
  std::vector<ObjectID> NodeObjects;
  std::map<ObjectID, std::size_t> Lane;
};

} // namespace ufg
} // namespace lotus
