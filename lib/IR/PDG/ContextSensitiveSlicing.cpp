/**
 * @file ContextSensitiveSlicing.cpp
 * @brief Implementation of context-sensitive slicing using CFL-reachability
 *
 * This file implements context-sensitive slicing primitives for the Program Dependency Graph
 * using Context-Free Language (CFL) reachability. The key innovation is maintaining a call
 * stack during traversal to ensure that data flows are tracked correctly across function
 * boundaries, preventing spurious dependencies from merging at function entry/exit points.
 *
 * CFL-Reachability Algorithm:
 * - Uses a context-free grammar to define valid paths through the program
 * - Call edges (CONTROLDEP_CALLINV) push call sites onto the stack
 * - Return edges (CONTROLDEP_CALLRET) pop matching call sites from the stack
 * - Only allows traversal when call/return pairs are properly matched
 * - Prevents data flows that cross unmatched call/return boundaries
 *
 * Features:
 * - Context-sensitive forward slicing: Find all nodes reachable from criteria with proper call/return matching
 * - Context-sensitive backward slicing: Find all nodes that can reach criteria with proper call/return matching
 * - Context-sensitive program chopping: Find nodes on paths between source and sink with context sensitivity
 * - Call stack tracking: Maintains stack of call sites to match with return sites
 * - CFL-reachability: Uses context-free language reachability for precise inter-procedural analysis
 * - Edge type filtering: Configurable edge type filtering for different analysis needs
 * - Proper call-return matching: Uses PDG's existing call-return edge connections
 */

#include "IR/PDG/ContextSensitiveSlicing.h"
#include "IR/PDG/PDGUtils.h"
#include <queue>

using namespace llvm;

namespace pdg
{

// ==================== ContextSensitiveSlicing Implementation ====================

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeForwardSlice(Node &start_node, const std::set<EdgeType> &edge_types)
{
  return computeForwardSlice({&start_node}, edge_types);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeForwardSlice(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types)
{
  return traverseWithStack(start_nodes, edge_types, true);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeBackwardSlice(Node &end_node, const std::set<EdgeType> &edge_types)
{
  return computeBackwardSlice({&end_node}, edge_types);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeBackwardSlice(const NodeSet &end_nodes, const std::set<EdgeType> &edge_types)
{
  return traverseWithStack(end_nodes, edge_types, false);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeChop(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types)
{
  NodeSet forward_slice = computeForwardSlice(source_node, edge_types);
  NodeSet backward_slice = computeBackwardSlice(sink_node, edge_types);
  
  NodeSet chop;
  for (auto *node : forward_slice) {
    if (backward_slice.count(node)) {
      chop.insert(node);
    }
  }
  return chop;
}

bool ContextSensitiveSlicing::hasContextSensitivePath(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types)
{
  return !computeChop(source_node, sink_node, edge_types).empty();
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::traverseWithStack(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types, bool forward)
{
  NodeSet slice;
  VisitedSet visited;
  std::queue<std::pair<Node *, std::vector<Node *>>> worklist; // <node, call_stack>
  
  // Initialize worklist with starting nodes and empty call stacks
  for (auto *node : start_nodes) {
    if (node != nullptr) {
      worklist.push({node, std::vector<Node *>()});
      slice.insert(node);
    }
  }
  
  // BFS traversal with CFL-reachability constraints
  // Safety limit prevents infinite loops in case of cycles
  for (size_t iteration_count = 0; !worklist.empty() && iteration_count < 50000; ++iteration_count) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    std::vector<Node *> call_stack = current_pair.second;
    worklist.pop();
    
    // Check if this (node, call_stack) state has been visited
    auto state = std::make_pair(current, call_stack);
    if (visited.find(state) != visited.end()) {
      continue;
    }
    visited.insert(state);
    
    try {
      auto &edges = forward ? current->getOutEdgeSet() : current->getInEdgeSet();
      for (auto *edge : edges) {
        // Skip null edges or edges not in allowed types
        if (edge == nullptr || (!edge_types.empty() && edge_types.find(edge->getEdgeType()) == edge_types.end())) {
          continue;
        }
        
        Node *neighbor = forward ? edge->getDstNode() : edge->getSrcNode();
        if (neighbor == nullptr) {
          continue;
        }
        
        // Create a copy of the call stack for this traversal path
        std::vector<Node *> new_stack = call_stack;
        
        if (forward) {
          // Forward traversal: call edges push, return edges pop
          if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLINV && current->getNodeType() == GraphNodeType::INST_FUNCALL) {
            // Push call site onto stack when traversing call invocation edge
            new_stack.push_back(current);
          } else if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLRET) {
            // Pop matching call site from stack when traversing return edge
            if (!new_stack.empty() && neighbor == new_stack.back()) {
              new_stack.pop_back();
            } else {
              // Skip this edge if call/return don't match (CFL-reachability constraint)
              continue;
            }
          }
        } else {
          // Backward traversal: return edges push, call edges pop
          if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLRET && current->getNodeType() == GraphNodeType::INST_RET) {
            // Push return site onto stack when traversing return edge backward
            new_stack.push_back(current);
          } else if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLINV) {
            // Pop matching return site from stack when traversing call edge backward
            if (!new_stack.empty() && neighbor == new_stack.back()) {
              new_stack.pop_back();
            } else {
              // Skip this edge if call/return don't match (CFL-reachability constraint)
              continue;
            }
          }
        }
        
        // Check if this (node, call_stack) state has been visited
        auto new_state = std::make_pair(neighbor, new_stack);
        if (visited.find(new_state) == visited.end()) {
          // Add neighbor to slice and queue for further exploration
          slice.insert(neighbor);
          worklist.push({neighbor, new_stack});
        }
      }
    } catch (...) {
      // Skip this node if there's an error accessing its edges
      continue;
    }
  }
  
  return slice;
}


// Note: getAssociatedCallNode is no longer needed in the full CFL-reachability implementation
// The PDG already connects return instructions directly to their corresponding call sites
// via CONTROLDEP_CALLRET edges, so we can use direct edge traversal for proper CFL-reachability

// ==================== ContextSensitiveSlicingUtils Implementation ====================

std::set<EdgeType> ContextSensitiveSlicingUtils::getCallReturnEdges()
{
  return {
    EdgeType::CONTROLDEP_CALLINV,
    EdgeType::CONTROLDEP_CALLRET,
    EdgeType::PARAMETER_IN,
    EdgeType::PARAMETER_OUT,
    EdgeType::DATA_RET
  };
}

std::unordered_map<std::string, size_t> ContextSensitiveSlicingUtils::compareSlices(const NodeSet &cs_slice, const NodeSet &ci_slice)
{
  std::unordered_map<std::string, size_t> comparison;
  comparison["cs_slice_size"] = cs_slice.size();
  comparison["ci_slice_size"] = ci_slice.size();
  
  size_t cs_only = 0, ci_only = 0, common = 0;
  for (auto *node : cs_slice) {
    if (ci_slice.find(node) == ci_slice.end()) {
      cs_only++;
    } else {
      common++;
    }
  }
  for (auto *node : ci_slice) {
    if (cs_slice.find(node) == cs_slice.end()) {
      ci_only++;
    }
  }
  
  comparison["cs_only_nodes"] = cs_only;
  comparison["ci_only_nodes"] = ci_only;
  comparison["common_nodes"] = common;
  
  if (ci_slice.size() > 0) {
    comparison["precision_improvement_percent"] = static_cast<size_t>((double)ci_only / ci_slice.size() * 100.0);
  }
  
  return comparison;
}

void ContextSensitiveSlicingUtils::printContextSensitiveSlice(const NodeSet &slice, const std::string &slice_name)
{
  errs() << "=============== Context-Sensitive " << slice_name << " ===============\n";
  errs() << "Total nodes: " << slice.size() << "\n";
  
  for (auto *node : slice) {
    if (node == nullptr) continue;
    errs() << "node: " << node << " - " << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
  }
  errs() << "==========================================\n";
}

std::unordered_map<std::string, size_t> ContextSensitiveSlicingUtils::getContextSensitiveSliceStatistics(const NodeSet &slice)
{
  std::unordered_map<std::string, size_t> stats;
  stats["total_nodes"] = slice.size();
  
  std::unordered_map<GraphNodeType, size_t> node_type_counts;
  std::unordered_map<EdgeType, size_t> edge_type_counts;
  size_t call_nodes = 0, return_nodes = 0, parameter_nodes = 0;
  
  for (auto *node : slice) {
    if (node == nullptr) continue;
    
    GraphNodeType node_type = node->getNodeType();
    node_type_counts[node_type]++;
    
    if (node_type == GraphNodeType::INST_FUNCALL) {
      call_nodes++;
    } else if (node_type == GraphNodeType::INST_RET) {
      return_nodes++;
    } else if (node_type == GraphNodeType::PARAM_FORMALIN || 
               node_type == GraphNodeType::PARAM_FORMALOUT ||
               node_type == GraphNodeType::PARAM_ACTUALIN ||
               node_type == GraphNodeType::PARAM_ACTUALOUT) {
      parameter_nodes++;
    }
    
    for (auto *edge : node->getInEdgeSet())
      edge_type_counts[edge->getEdgeType()]++;
    for (auto *edge : node->getOutEdgeSet())
      edge_type_counts[edge->getEdgeType()]++;
  }
  
  for (const auto &pair : node_type_counts)
    stats["node_type_" + pdgutils::getNodeTypeStr(pair.first)] = pair.second;
  for (const auto &pair : edge_type_counts)
    stats["edge_type_" + pdgutils::getEdgeTypeStr(pair.first)] = pair.second;
  
  stats["call_nodes"] = call_nodes;
  stats["return_nodes"] = return_nodes;
  stats["parameter_nodes"] = parameter_nodes;
  
  return stats;
}

std::unordered_map<std::string, size_t> ContextSensitiveSlicingUtils::getCFLReachabilityStatistics(const NodeSet &slice)
{
  std::unordered_map<std::string, size_t> stats;
  stats["total_nodes"] = slice.size();
  
  size_t call_nodes = 0, return_nodes = 0, matched_pairs = 0, unmatched_calls = 0, unmatched_returns = 0;
  
  for (auto *node : slice) {
    if (node == nullptr) continue;
    
    GraphNodeType node_type = node->getNodeType();
    if (node_type == GraphNodeType::INST_FUNCALL) {
      call_nodes++;
      bool has_return_edges = false;
      for (auto *edge : node->getOutEdgeSet()) {
        if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLRET) {
          has_return_edges = true;
          break;
        }
      }
      if (has_return_edges) matched_pairs++; else unmatched_calls++;
    } else if (node_type == GraphNodeType::INST_RET) {
      return_nodes++;
      bool has_call_edges = false;
      for (auto *edge : node->getInEdgeSet()) {
        if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLINV) {
          has_call_edges = true;
          break;
        }
      }
      if (!has_call_edges) unmatched_returns++;
    }
  }
  
  stats["call_nodes"] = call_nodes;
  stats["return_nodes"] = return_nodes;
  stats["matched_call_return_pairs"] = matched_pairs;
  stats["unmatched_calls"] = unmatched_calls;
  stats["unmatched_returns"] = unmatched_returns;
  
  if (call_nodes > 0) {
    stats["call_match_percentage"] = static_cast<size_t>((double)matched_pairs / call_nodes * 100.0);
  }
  
  return stats;
}

bool ContextSensitiveSlicingUtils::isCFLValidPath(const std::vector<Node *> &path, GenericGraph &/*pdg*/)
{
  if (path.empty()) return true;
  
  std::vector<Node *> call_stack;
  
  for (size_t i = 0; i < path.size() - 1; ++i) {
    Node *current = path[i];
    Node *next = path[i + 1];
    
    if (!current || !next) continue;
    
    EdgeType edge_type = EdgeType::TYPE_OTHEREDGE;
    for (auto *edge : current->getOutEdgeSet()) {
      if (edge && edge->getDstNode() == next) {
        edge_type = edge->getEdgeType();
        break;
      }
    }
    
    if (edge_type == EdgeType::CONTROLDEP_CALLINV) {
      call_stack.push_back(current);
    } else if (edge_type == EdgeType::CONTROLDEP_CALLRET) {
      if (call_stack.empty() || call_stack.back() != current) {
        return false;
      }
      call_stack.pop_back();
    }
  }
  
  return call_stack.empty();
}

} // namespace pdg
