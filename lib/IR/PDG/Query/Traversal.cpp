#include "Traversal.h"

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
  size_t stack_depth = 0;
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

// Context-sensitive stack transition semantics:
// Forward traversal pushes the callsite node on CONTROLDEP_CALLINV and only
// allows popping back to that exact callsite node on CONTROLDEP_CALLRET.
// Because any matched CALLINV..CALLRET returns to the exact same callsite node
// already visited at a strictly smaller depth and stack depth, every reachable
// node and shortest valid path uses zero CONTROLDEP_CALLRET edges in forward
// mode (and symmetrically zero CONTROLDEP_CALLINV edges in backward mode).
// Tracking stack_depth instead of explicit full call-stack vectors eliminates
// exponential/infinite blowup on DAG and recursive call graphs while preserving
// identical reachable nodes, edges, distances, and stack-depth bounds.
static bool applyCallStackDepthTransition(bool forward, EdgeType edge_type,
                                          size_t &stack_depth) {
  if (edge_type == EdgeType::CONTROLDEP_CALLINV) {
    if (forward) {
      ++stack_depth;
      return true;
    }
    return false;
  }

  if (edge_type == EdgeType::CONTROLDEP_CALLRET) {
    if (forward)
      return false;
    ++stack_depth;
    return true;
  }

  return true;
}

TraversalOutcome traverseGraph(ProgramGraph &pdg, const NodeSet &start_nodes,
                               const std::set<EdgeType> &edge_types,
                               const PDGQueryOptions &options, bool forward,
                               PDGQueryDiagnostics &diagnostics) {
  TraversalOutcome outcome;
  const bool has_scope_filter =
      options.scope.kind != PDGQueryScope::Kind::WholeGraph;
  const NodeSet scoped_nodes =
      has_scope_filter ? scopeNodes(pdg, options.scope) : NodeSet{};
  const bool track_stack_bound =
      options.context_mode == PDGContextMode::ContextSensitive &&
      options.limits.max_stack_depth > 0;
  std::queue<TraversalState> worklist;
  std::unordered_set<Node *> visited_nodes;
  std::unordered_map<Node *, size_t> best_stack_depth;

  for (Node *node : start_nodes) {
    if (node == nullptr)
      continue;
    if (has_scope_filter && scoped_nodes.count(node) == 0)
      diagnostics.unresolved_criteria.push_back("criteria node outside scope");
    outcome.nodes.insert(node);
    outcome.distances[node] = 0;
    if (track_stack_bound) {
      if (best_stack_depth.emplace(node, 0).second)
        worklist.push(TraversalState{node, 0, 0});
    } else {
      if (visited_nodes.insert(node).second)
        worklist.push(TraversalState{node, 0, 0});
    }
  }

  while (!worklist.empty()) {
    const TraversalState state = worklist.front();
    worklist.pop();

    diagnostics.explored_states++;
    diagnostics.max_depth_reached =
        std::max(diagnostics.max_depth_reached, state.depth);
    diagnostics.max_stack_depth_reached =
        std::max(diagnostics.max_stack_depth_reached, state.stack_depth);

    if (options.limits.max_states > 0 &&
        diagnostics.explored_states > options.limits.max_states) {
      diagnostics.state_limit_hit = true;
      break;
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

      size_t next_stack_depth = state.stack_depth;
      if (options.context_mode == PDGContextMode::ContextSensitive &&
          !applyCallStackDepthTransition(forward, edge_type,
                                         next_stack_depth)) {
        continue;
      }

      if (options.limits.max_stack_depth > 0 &&
          next_stack_depth > options.limits.max_stack_depth) {
        diagnostics.stack_depth_limit_hit = true;
        continue;
      }

      if (has_scope_filter && scoped_nodes.count(neighbor) == 0)
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
      auto &pred_edges = outcome.predecessor_edges[neighbor];
      if (pred_edges.empty())
        pred_edges.push_back(std::make_pair(state.node, edge_type));

      const size_t next_depth = state.depth + 1;
      std::unordered_map<Node *, size_t>::iterator dist_it =
          outcome.distances.find(neighbor);
      if (dist_it == outcome.distances.end() || next_depth < dist_it->second)
        outcome.distances[neighbor] = next_depth;

      bool should_enqueue = false;
      if (track_stack_bound) {
        auto bit = best_stack_depth.find(neighbor);
        if (bit == best_stack_depth.end() || next_stack_depth < bit->second) {
          best_stack_depth[neighbor] = next_stack_depth;
          should_enqueue = true;
        }
      } else {
        should_enqueue = visited_nodes.insert(neighbor).second;
      }
      if (should_enqueue) {
        worklist.push(TraversalState{neighbor, next_depth, next_stack_depth});
      }
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

static void fillWitnessPaths(
    PDGQueryResult &result,
    const std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
        &predecessor_edges,
    size_t max_paths) {
  if (result.criteria_nodes.empty())
    return;

  std::unordered_set<std::string> seen_paths;
  for (NodeSet::const_iterator it = result.nodes.begin();
       it != result.nodes.end(); ++it) {
    if (max_paths > 0 && result.witness_paths.size() >= max_paths)
      break;
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
      result.witness_paths.push_back(std::move(witness));
  }
}

PDGQueryResult resultFromTraversal(const NodeSet &criteria_nodes,
                                   const TraversalOutcome &outcome,
                                   PDGQueryDiagnostics diagnostics,
                                   bool explain, size_t max_paths) {
  PDGQueryResult result;
  result.criteria_nodes = criteria_nodes;
  result.nodes = outcome.nodes;
  result.edges = outcome.edges;
  result.predecessors = outcome.predecessors;
  result.distances = outcome.distances;
  result.diagnostics = diagnostics;
  if (explain)
    fillWitnessPaths(result, outcome.predecessor_edges, max_paths);
  return result;
}

} // namespace pdg::query_detail
