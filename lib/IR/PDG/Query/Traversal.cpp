#include "IR/PDG/Query/Traversal.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <queue>
#include <unordered_set>

using namespace llvm;

namespace pdg::query_detail {

struct TraversalState {
  Node *node = nullptr;
  size_t depth = 0;
  std::vector<Node *> call_stack;
};

struct StateHash {
  size_t operator()(const std::pair<Node *, std::vector<Node *>> &value) const {
    size_t seed = std::hash<Node *>()(value.first);
    for (Node *node : value.second) {
      seed ^=
          std::hash<Node *>()(node) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

static bool isControlEdge(EdgeType type) {
  return type == EdgeType::CONTROLDEP_CALLINV ||
         type == EdgeType::CONTROLDEP_CALLRET ||
         type == EdgeType::CONTROLDEP_ENTRY ||
         type == EdgeType::CONTROLDEP_BR || type == EdgeType::CONTROLDEP_IND_BR;
}

static bool usesValueAsPointerOperand(const Value *value,
                                      const Instruction *user) {
  if (value == nullptr || user == nullptr)
    return false;
  if (const auto *load = dyn_cast<LoadInst>(user))
    return load->getPointerOperand() == value;
  if (const auto *store = dyn_cast<StoreInst>(user))
    return store->getPointerOperand() == value;
  if (const auto *gep = dyn_cast<GetElementPtrInst>(user))
    return gep->getPointerOperand() == value;
  return false;
}

static bool isFieldAccess(Node *node) {
  if (node == nullptr || node->getValue() == nullptr)
    return false;
  return isa<LoadInst>(node->getValue()) || isa<StoreInst>(node->getValue()) ||
         isa<GetElementPtrInst>(node->getValue());
}

static bool isValueFlowEdge(Edge *edge, Node *src_node, Node *dst_node) {
  if (edge == nullptr || src_node == nullptr || dst_node == nullptr)
    return false;

  switch (edge->getEdgeType()) {
  case EdgeType::DATA_RET:
  case EdgeType::VAL_DEP:
  case EdgeType::DATA_ALIAS:
  case EdgeType::PARAMETER_IN:
  case EdgeType::PARAMETER_OUT:
  case EdgeType::PARAMETER_FIELD:
    return true;
  case EdgeType::DATA_DEF_USE:
  case EdgeType::DATA_RAW:
  case EdgeType::DATA_READ:
    break;
  default:
    return false;
  }

  const Value *src_value = src_node->getValue();
  const Value *dst_value = dst_node->getValue();
  const Instruction *dst_inst = dyn_cast_or_null<Instruction>(dst_value);
  const Instruction *src_inst = dyn_cast_or_null<Instruction>(src_value);

  if (usesValueAsPointerOperand(src_value, dst_inst))
    return false;

  if (const auto *src_gep = dyn_cast_or_null<GetElementPtrInst>(src_value)) {
    if (usesValueAsPointerOperand(src_gep, dst_inst))
      return false;
  }

  if (const auto *dst_store = dyn_cast_or_null<StoreInst>(dst_value))
    return dst_store->getValueOperand() == src_value;

  if (const auto *dst_load = dyn_cast_or_null<LoadInst>(dst_value))
    return dst_load->getPointerOperand() != src_value;

  if (const auto *dst_gep = dyn_cast_or_null<GetElementPtrInst>(dst_value))
    return dst_gep->getPointerOperand() != src_value;

  if (const auto *src_store = dyn_cast_or_null<StoreInst>(src_value)) {
    if (dst_value != nullptr && src_store->getPointerOperand() == dst_value)
      return false;
  }

  return src_inst != nullptr || dst_inst != nullptr;
}

static bool applyCallStackTransition(bool forward, Node *current,
                                     Node *neighbor, EdgeType edge_type,
                                     std::vector<Node *> &call_stack) {
  if (edge_type == EdgeType::CONTROLDEP_CALLINV) {
    if (forward) {
      call_stack.push_back(current);
      return true;
    }
    if (call_stack.empty() || neighbor != call_stack.back())
      return false;
    call_stack.pop_back();
    return true;
  }

  if (edge_type == EdgeType::CONTROLDEP_CALLRET) {
    if (forward) {
      if (call_stack.empty() || neighbor != call_stack.back())
        return false;
      call_stack.pop_back();
      return true;
    }
    call_stack.push_back(current);
    return true;
  }

  return true;
}

static bool shouldRecordNode(const NodeSet &scope, Node *node) {
  return scope.empty() || scope.count(node) != 0;
}

TraversalOutcome traverseGraph(ProgramGraph &pdg, const NodeSet &start_nodes,
                               const std::set<EdgeType> &edge_types,
                               const PDGQueryOptions &options, bool forward,
                               PDGQueryDiagnostics &diagnostics) {
  TraversalOutcome outcome;
  const NodeSet scoped_nodes = scopeNodes(pdg, options.scope);
  std::queue<TraversalState> worklist;
  std::unordered_set<Node *> visited_nodes;
  std::unordered_set<std::pair<Node *, std::vector<Node *>>, StateHash>
      visited_states;

  for (Node *node : start_nodes) {
    if (node == nullptr)
      continue;
    if (!shouldRecordNode(scoped_nodes, node))
      diagnostics.unresolved_criteria.push_back("criteria node outside scope");
    outcome.nodes.insert(node);
    outcome.distances[node] = 0;
    worklist.push(TraversalState{node, 0, std::vector<Node *>()});
  }

  while (!worklist.empty()) {
    const TraversalState state = worklist.front();
    worklist.pop();

    diagnostics.explored_states++;
    diagnostics.max_depth_reached =
        std::max(diagnostics.max_depth_reached, state.depth);
    diagnostics.max_stack_depth_reached =
        std::max(diagnostics.max_stack_depth_reached, state.call_stack.size());

    if (options.limits.max_states > 0 &&
        diagnostics.explored_states > options.limits.max_states) {
      diagnostics.state_limit_hit = true;
      break;
    }

    if (options.context_mode == PDGContextMode::ContextSensitive) {
      const std::pair<Node *, std::vector<Node *>> key(state.node,
                                                       state.call_stack);
      if (visited_states.count(key) != 0)
        continue;
      visited_states.insert(key);
    } else {
      if (visited_nodes.count(state.node) != 0)
        continue;
      visited_nodes.insert(state.node);
    }

    if (options.limits.max_depth > 0 &&
        state.depth >= options.limits.max_depth) {
      diagnostics.depth_limit_hit = true;
      continue;
    }

    const bool current_is_field_access = isFieldAccess(state.node);
    const Node::EdgeSet &edges =
        forward ? state.node->getOutEdgeSet() : state.node->getInEdgeSet();

    for (Node::EdgeSet::const_iterator it = edges.begin(); it != edges.end();
         ++it) {
      Edge *edge = *it;
      if (edge == nullptr)
        continue;

      Node *neighbor = forward ? edge->getDstNode() : edge->getSrcNode();
      if (neighbor == nullptr)
        continue;

      const EdgeType edge_type = edge->getEdgeType();
      if (!isEdgeAllowed(edge_type, edge_types))
        continue;

      std::vector<Node *> next_stack = state.call_stack;
      if (options.context_mode == PDGContextMode::ContextSensitive &&
          !applyCallStackTransition(forward, state.node, neighbor, edge_type,
                                    next_stack)) {
        continue;
      }

      if (options.limits.max_stack_depth > 0 &&
          next_stack.size() > options.limits.max_stack_depth) {
        diagnostics.stack_depth_limit_hit = true;
        continue;
      }

      if (!scoped_nodes.empty() && scoped_nodes.count(neighbor) == 0)
        continue;

      if (options.slice_flavor == SliceFlavor::Thin) {
        if (isControlEdge(edge_type))
          continue;
        if ((current_is_field_access && !forward &&
             !isValueFlowEdge(edge, neighbor, state.node)) ||
            (isFieldAccess(neighbor) && forward &&
             !isValueFlowEdge(edge, state.node, neighbor))) {
          continue;
        }
      }

      outcome.nodes.insert(neighbor);
      outcome.edges.insert(edge);
      outcome.predecessors[neighbor].insert(state.node);
      outcome.predecessor_edges[neighbor].push_back(
          std::make_pair(state.node, edge_type));

      const size_t next_depth = state.depth + 1;
      std::unordered_map<Node *, size_t>::iterator dist_it =
          outcome.distances.find(neighbor);
      if (dist_it == outcome.distances.end() || next_depth < dist_it->second)
        outcome.distances[neighbor] = next_depth;

      TraversalState next_state;
      next_state.node = neighbor;
      next_state.depth = next_depth;
      next_state.call_stack = next_stack;
      worklist.push(next_state);
    }
  }

  outcome.edges = collectInducedEdges(outcome.nodes, edge_types);
  return outcome;
}

static std::vector<Node *> materializePath(
    Node *node,
    const std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
        &predecessor_edges,
    const NodeSet &start_nodes, std::vector<EdgeType> *edge_types) {
  std::vector<Node *> path;
  std::vector<EdgeType> local_edge_types;
  Node *current = node;
  std::unordered_set<Node *> seen;

  while (current != nullptr && seen.insert(current).second) {
    path.push_back(current);
    if (start_nodes.count(current) != 0)
      break;
    std::unordered_map<
        Node *, std::vector<std::pair<Node *, EdgeType>>>::const_iterator it =
        predecessor_edges.find(current);
    if (it == predecessor_edges.end() || it->second.empty())
      break;
    local_edge_types.push_back(it->second.front().second);
    current = it->second.front().first;
  }

  std::reverse(path.begin(), path.end());
  std::reverse(local_edge_types.begin(), local_edge_types.end());
  if (edge_types != nullptr)
    *edge_types = local_edge_types;
  return path;
}

static void fillWitnessPaths(PDGQueryResult &result) {
  if (result.criteria_nodes.empty())
    return;

  std::set<std::string> seen_paths;
  std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
      predecessor_edges;
  for (std::unordered_map<Node *, std::set<Node *>>::const_iterator it =
           result.predecessors.begin();
       it != result.predecessors.end(); ++it) {
    for (std::set<Node *>::const_iterator pred_it = it->second.begin();
         pred_it != it->second.end(); ++pred_it) {
      predecessor_edges[it->first].push_back(
          std::make_pair(*pred_it, EdgeType::TYPE_OTHEREDGE));
    }
  }

  for (NodeSet::const_iterator it = result.nodes.begin();
       it != result.nodes.end(); ++it) {
    Node *node = *it;
    if (node == nullptr || result.criteria_nodes.count(node) != 0)
      continue;
    PDGWitnessPath witness;
    witness.kind = PDGWitnessPathKind::Slice;
    witness.nodes = materializePath(node, predecessor_edges,
                                    result.criteria_nodes, &witness.edge_types);
    if (witness.nodes.size() < 2)
      continue;
    const std::string key = pathKey(witness);
    if (seen_paths.insert(key).second)
      result.witness_paths.push_back(witness);
  }
}

PDGQueryResult resultFromTraversal(const NodeSet &criteria_nodes,
                                   const TraversalOutcome &outcome,
                                   PDGQueryDiagnostics diagnostics,
                                   bool explain) {
  PDGQueryResult result;
  result.criteria_nodes = criteria_nodes;
  result.nodes = outcome.nodes;
  result.edges = outcome.edges;
  result.predecessors = outcome.predecessors;
  result.distances = outcome.distances;
  result.diagnostics = diagnostics;
  if (explain)
    fillWitnessPaths(result);
  return result;
}

} // namespace pdg::query_detail
