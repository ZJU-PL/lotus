#include "IR/PDG/Query/DataFlowQuery.h"

#include "IR/PDG/Query/SliceQuery.h"
#include "QuerySupport.h"

namespace pdg {
using namespace llvm;
using namespace query_detail;

PDGQueryResult
DataFlowQuery::reachingDefinitions(const PDGCriteria &uses,
                                   const PDGQueryOptions &options,
                                   const Module *module) const {
  PDGQueryOptions data_options = options;
  data_options.edge_preset = PDGEdgePreset::Data;
  SliceQuery slice(pdg_);
  return slice.backward(uses, data_options, module);
}

std::vector<DefUseLink>
DataFlowQuery::defUseChain(Node &definition,
                           const PDGQueryOptions &options) const {
  std::vector<DefUseLink> chain;
  PDGQueryOptions local_options = options;
  local_options.edge_preset = PDGEdgePreset::Data;
  PDGCriteria criteria;
  criteria.nodes.insert(&definition);
  PDGQueryResult result = reachingDefinitions(criteria, local_options, nullptr);
  for (NodeSet::const_iterator it = result.nodes.begin();
       it != result.nodes.end(); ++it) {
    Node *node = *it;
    if (node == &definition)
      continue;
    if (result.predecessors.count(node) == 0)
      continue;
    for (std::set<Node *>::const_iterator pred_it =
             result.predecessors[node].begin();
         pred_it != result.predecessors[node].end(); ++pred_it) {
      if (*pred_it == nullptr)
        continue;
      chain.push_back(DefUseLink{*pred_it, node, edgeBetween(*pred_it, node)});
    }
  }
  return chain;
}

std::vector<DefUseLink>
DataFlowQuery::useDefChain(Node &use, const PDGQueryOptions &options) const {
  std::vector<DefUseLink> chain;
  PDGQueryOptions local_options = options;
  local_options.edge_preset = PDGEdgePreset::Data;
  PDGCriteria criteria;
  criteria.nodes.insert(&use);
  SliceQuery slice(pdg_);
  PDGQueryResult result = slice.backward(criteria, local_options, nullptr);
  for (std::unordered_map<Node *, std::set<Node *>>::const_iterator it =
           result.predecessors.begin();
       it != result.predecessors.end(); ++it) {
    for (std::set<Node *>::const_iterator pred_it = it->second.begin();
         pred_it != it->second.end(); ++pred_it) {
      if (*pred_it == nullptr)
        continue;
      chain.push_back(
          DefUseLink{*pred_it, it->first, edgeBetween(*pred_it, it->first)});
    }
  }
  return chain;
}

PDGQueryResult DataFlowQuery::liveNodes(const PDGQueryOptions &options) const {
  const bool whole_graph =
      options.scope.kind == PDGQueryScope::Kind::WholeGraph;
  const NodeSet nodes = whole_graph ? NodeSet{} : scopeNodes(pdg_, options.scope);
  const std::set<EdgeType> edge_types = edgeTypesForPreset(PDGEdgePreset::Data);
  PDGQueryResult result;
  auto checkNode = [&](Node *node) {
    if (!node)
      return;
    for (Edge *edge : node->getOutEdgeSet()) {
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *dst = edge->getDstNode();
      if (dst != nullptr && (whole_graph || nodes.count(dst) != 0)) {
        result.nodes.insert(node);
        break;
      }
    }
  };
  if (whole_graph) {
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it)
      checkNode(*it);
  } else {
    for (Node *node : nodes)
      checkNode(node);
  }
  result.edges = collectInducedEdges(result.nodes, edge_types);
  return result;
}

PDGQueryResult DataFlowQuery::deadNodes(const PDGQueryOptions &options) const {
  const bool whole_graph =
      options.scope.kind == PDGQueryScope::Kind::WholeGraph;
  const NodeSet nodes = whole_graph ? NodeSet{} : scopeNodes(pdg_, options.scope);
  const std::set<EdgeType> edge_types = edgeTypesForPreset(PDGEdgePreset::Data);
  PDGQueryResult result;
  auto checkDead = [&](Node *node) {
    if (!node)
      return;
    for (Edge *edge : node->getOutEdgeSet()) {
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *dst = edge->getDstNode();
      if (dst != nullptr && (whole_graph || nodes.count(dst) != 0))
        return;
    }
    result.nodes.insert(node);
  };
  if (whole_graph) {
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it)
      checkDead(*it);
  } else {
    for (Node *node : nodes)
      checkDead(node);
  }
  result.edges = collectInducedEdges(result.nodes, edge_types);
  return result;
}

std::vector<ControllingCondition>
DataFlowQuery::immediateControllers(Node &node) const {
  std::vector<ControllingCondition> controllers;
  const std::set<EdgeType> edge_types =
      edgeTypesForPreset(PDGEdgePreset::Control);
  for (Node::EdgeSet::const_iterator it = node.getInEdgeSet().begin();
       it != node.getInEdgeSet().end(); ++it) {
    Edge *edge = *it;
    if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
      continue;
    controllers.push_back(
        ControllingCondition{edge->getSrcNode(), edge->getEdgeType()});
  }
  return controllers;
}

PDGQueryResult DataFlowQuery::allControllers(const PDGCriteria &criteria,
                                             const PDGQueryOptions &options,
                                             const Module *module) const {
  PDGQueryOptions control_options = options;
  control_options.edge_preset = PDGEdgePreset::Control;
  SliceQuery slice(pdg_);
  return slice.backward(criteria, control_options, module);
}

PDGQueryResult DataFlowQuery::controlRegion(const PDGCriteria &criteria,
                                            const PDGQueryOptions &options,
                                            const Module *module) const {
  PDGQueryOptions control_options = options;
  control_options.edge_preset = PDGEdgePreset::Control;
  SliceQuery slice(pdg_);
  return slice.forward(criteria, control_options, module);
}

} // namespace pdg
