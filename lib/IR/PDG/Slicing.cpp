/**
 * @file Slicing.cpp
 * @brief Implementation of slicing primitives for the Program Dependency Graph
 *
 * This file implements forward slicing, backward slicing, and program chopping
 * primitives over the PDG. These primitives enable various program analysis
 * tasks such as impact analysis, debugging, and program understanding.
 *
 * Features:
 * - Forward slicing: Find all nodes reachable from a given set of criteria
 * - Backward slicing: Find all nodes that can reach a given set of criteria  
 * - Program chopping: Find all nodes on paths between source and sink nodes
 * - Configurable edge type filtering
 * - Depth-limited traversal
 * - Path enumeration for chopping
 */

#include "IR/PDG/Slicing.h"
#include "IR/PDG/PDGUtils.h"
#include <queue>

using namespace llvm;

namespace pdg
{

// ==================== ForwardSlicing Implementation ====================

ForwardSlicing::NodeSet ForwardSlicing::computeSlice(Node &start_node, const std::set<EdgeType> &edge_types)
{
  return computeSlice({&start_node}, edge_types);
}

ForwardSlicing::NodeSet ForwardSlicing::computeSlice(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types)
{
  NodeSet slice;
  VisitedSet visited;
  std::queue<Node *> worklist;
  
  // Initialize worklist with start nodes
  for (auto *node : start_nodes)
    if (node != nullptr) {
      worklist.push(node);
      slice.insert(node);
    }
  
  // BFS traversal to find all reachable nodes
  // Add safety limit to prevent infinite loops
  for (size_t iteration_count = 0; !worklist.empty() && iteration_count < 10000; ++iteration_count) {
    Node *current = worklist.front();
    worklist.pop();
    
    if (visited.find(current) != visited.end())
      continue;
    
    visited.insert(current);
    
    // Explore all outgoing edges
    try {
      for (auto *edge : current->getOutEdgeSet()) {
        if (edge == nullptr || !isEdgeTypeAllowed(edge->getEdgeType(), edge_types))
          continue;
          
        Node *neighbor = edge->getDstNode();
        if (neighbor == nullptr || visited.find(neighbor) != visited.end())
          continue;
        
        slice.insert(neighbor);
        worklist.push(neighbor);
      }
    } catch (...) {
      // Skip this node if there's an error accessing its edges
      continue;
    }
  }
  
  return slice;
}

ForwardSlicing::NodeSet ForwardSlicing::computeSliceWithDepth(Node &start_node, size_t max_depth, const std::set<EdgeType> &edge_types)
{
  if (max_depth == 0)
    return computeSlice(start_node, edge_types);
  
  NodeSet slice;
  VisitedSet visited;
  std::queue<std::pair<Node *, size_t>> worklist; // <node, depth>
  
  worklist.push({&start_node, 0});
  slice.insert(&start_node);
  
  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    size_t depth = current_pair.second;
    worklist.pop();
    
    if (visited.find(current) != visited.end() || depth >= max_depth)
      continue;
    
    visited.insert(current);
    
    // Explore all outgoing edges
    for (auto *edge : current->getOutEdgeSet()) {
      if (!isEdgeTypeAllowed(edge->getEdgeType(), edge_types))
        continue;
        
      Node *neighbor = edge->getDstNode();
      if (neighbor == nullptr || visited.find(neighbor) != visited.end())
        continue;
      
      slice.insert(neighbor);
      worklist.push({neighbor, depth + 1});
    }
  }
  
  return slice;
}

bool ForwardSlicing::isEdgeTypeAllowed(EdgeType edge_type, const std::set<EdgeType> &allowed_types) const
{
  return allowed_types.empty() || allowed_types.find(edge_type) != allowed_types.end();
}

// ==================== BackwardSlicing Implementation ====================

BackwardSlicing::NodeSet BackwardSlicing::computeSlice(Node &end_node, const std::set<EdgeType> &edge_types)
{
  return computeSlice({&end_node}, edge_types);
}

BackwardSlicing::NodeSet BackwardSlicing::computeSlice(const NodeSet &end_nodes, const std::set<EdgeType> &edge_types)
{
  NodeSet slice;
  VisitedSet visited;
  std::queue<Node *> worklist;
  
  // Initialize worklist with end nodes
  for (auto *node : end_nodes)
    if (node != nullptr) {
      worklist.push(node);
      slice.insert(node);
    }
  
  // BFS traversal to find all nodes that can reach the end nodes
  // Add safety limit to prevent infinite loops
  for (size_t iteration_count = 0; !worklist.empty() && iteration_count < 10000; ++iteration_count) {
    Node *current = worklist.front();
    worklist.pop();
    
    if (visited.find(current) != visited.end())
      continue;
    
    visited.insert(current);
    
    // Explore all incoming edges (backward traversal)
    try {
      for (auto *edge : current->getInEdgeSet()) {
        if (edge == nullptr || !isEdgeTypeAllowed(edge->getEdgeType(), edge_types))
          continue;
          
        Node *neighbor = edge->getSrcNode();
        if (neighbor == nullptr || visited.find(neighbor) != visited.end())
          continue;
        
        slice.insert(neighbor);
        worklist.push(neighbor);
      }
    } catch (...) {
      // Skip this node if there's an error accessing its edges
      continue;
    }
  }
  
  return slice;
}

BackwardSlicing::NodeSet BackwardSlicing::computeSliceWithDepth(Node &end_node, size_t max_depth, const std::set<EdgeType> &edge_types)
{
  if (max_depth == 0)
    return computeSlice(end_node, edge_types);
  
  NodeSet slice;
  VisitedSet visited;
  std::queue<std::pair<Node *, size_t>> worklist; // <node, depth>
  
  worklist.push({&end_node, 0});
  slice.insert(&end_node);
  
  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    size_t depth = current_pair.second;
    worklist.pop();
    
    if (visited.find(current) != visited.end() || depth >= max_depth)
      continue;
    
    visited.insert(current);
    
    // Explore all incoming edges (backward traversal)
    for (auto *edge : current->getInEdgeSet()) {
      if (!isEdgeTypeAllowed(edge->getEdgeType(), edge_types))
        continue;
        
      Node *neighbor = edge->getSrcNode();
      if (neighbor == nullptr || visited.find(neighbor) != visited.end())
        continue;
      
      slice.insert(neighbor);
      worklist.push({neighbor, depth + 1});
    }
  }
  
  return slice;
}

bool BackwardSlicing::isEdgeTypeAllowed(EdgeType edge_type, const std::set<EdgeType> &allowed_types) const
{
  return allowed_types.empty() || allowed_types.find(edge_type) != allowed_types.end();
}

// ==================== ProgramChopping Implementation ====================

ProgramChopping::NodeSet ProgramChopping::computeChop(const NodeSet &source_nodes, const NodeSet &sink_nodes, const std::set<EdgeType> &edge_types)
{
  NodeSet chop;
  
  // For each source-sink pair, find all nodes on paths between them
  for (auto *source : source_nodes)
    for (auto *sink : sink_nodes) {
      if (source == nullptr || sink == nullptr)
        continue;
        
      // Find all paths from source to sink
      auto paths = findAllPaths(*source, *sink, 0, edge_types);
      
      // Add all nodes on these paths to the chop
      for (const auto &path : paths)
        for (auto *node : path)
          chop.insert(node);
    }
  
  return chop;
}

ProgramChopping::NodeSet ProgramChopping::computeChop(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types)
{
  return computeChop({&source_node}, {&sink_node}, edge_types);
}

ProgramChopping::NodeSet ProgramChopping::computeChopWithDepth(const NodeSet &source_nodes, const NodeSet &sink_nodes, size_t max_depth, const std::set<EdgeType> &edge_types)
{
  NodeSet chop;
  
  // For each source-sink pair, find all nodes on paths between them
  for (auto *source : source_nodes)
    for (auto *sink : sink_nodes) {
      if (source == nullptr || sink == nullptr)
        continue;
        
      // Find all paths from source to sink with depth limit
      auto paths = findAllPaths(*source, *sink, max_depth, edge_types);
      
      // Add all nodes on these paths to the chop
      for (const auto &path : paths)
        if (path.size() <= max_depth + 1) // +1 because we count nodes, not edges
          for (auto *node : path)
            chop.insert(node);
    }
  
  return chop;
}

bool ProgramChopping::hasPath(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types)
{
  // Convert edge_types to exclude set for canReach
  std::set<EdgeType> exclude_edges;
  if (!edge_types.empty()) {
    // If specific edge types are requested, exclude all others
    // This is a simplified approach - in practice, canReach might need modification
    // to support inclusion-based filtering rather than exclusion-based
    for (int i = 0; i < static_cast<int>(EdgeType::TYPE_OTHEREDGE); ++i) {
      EdgeType et = static_cast<EdgeType>(i);
      if (edge_types.find(et) == edge_types.end())
        exclude_edges.insert(et);
    }
  }
  
  return _pdg.canReach(source_node, sink_node, exclude_edges);
}

std::vector<std::vector<Node *>> ProgramChopping::findAllPaths(Node &source_node, Node &sink_node, size_t max_paths, const std::set<EdgeType> &edge_types)
{
  std::vector<std::vector<Node *>> all_paths;
  std::vector<Node *> current_path;
  VisitedSet visited;
  
  findPathsDFS(source_node, sink_node, visited, current_path, all_paths, max_paths, edge_types);
  return all_paths;
}

void ProgramChopping::findPathsDFS(Node &current, Node &sink, VisitedSet &visited,
                                 std::vector<Node *> &current_path, std::vector<std::vector<Node *>> &all_paths,
                                 size_t max_paths, const std::set<EdgeType> &edge_types)
{
  // Add current node to path and mark as visited
  current_path.push_back(&current);
  visited.insert(&current);
  
  // If we reached the sink, add this path to results
  if (&current == &sink) {
    all_paths.push_back(current_path);
    current_path.pop_back();
    visited.erase(&current);
    return;
  }
  
  // If we've found enough paths, stop searching
  if (max_paths > 0 && all_paths.size() >= max_paths) {
    current_path.pop_back();
    visited.erase(&current);
    return;
  }
  
  // Prevent infinite loops by limiting path length
  const size_t MAX_PATH_LENGTH = 100;
  if (current_path.size() > MAX_PATH_LENGTH) {
    current_path.pop_back();
    visited.erase(&current);
    return;
  }
  
  // Explore all outgoing edges
  for (auto *edge : current.getOutEdgeSet()) {
    if (!isEdgeTypeAllowed(edge->getEdgeType(), edge_types))
      continue;
      
    Node *neighbor = edge->getDstNode();
    if (neighbor == nullptr || visited.find(neighbor) != visited.end())
      continue;
    
    findPathsDFS(*neighbor, sink, visited, current_path, all_paths, max_paths, edge_types);
  }
  
  // Backtrack
  current_path.pop_back();
  visited.erase(&current);
}

bool ProgramChopping::isEdgeTypeAllowed(EdgeType edge_type, const std::set<EdgeType> &allowed_types) const
{
  return allowed_types.empty() || allowed_types.find(edge_type) != allowed_types.end();
}

// ==================== SlicingUtils Implementation ====================

std::set<EdgeType> SlicingUtils::getDataDependencyEdges()
{
  return {
    EdgeType::DATA_DEF_USE,
    EdgeType::DATA_RAW,
    EdgeType::DATA_READ,
    EdgeType::DATA_ALIAS,
    EdgeType::DATA_RET,
    EdgeType::PARAMETER_IN,
    EdgeType::PARAMETER_OUT,
    EdgeType::PARAMETER_FIELD,
    EdgeType::VAL_DEP
  };
}

std::set<EdgeType> SlicingUtils::getControlDependencyEdges()
{
  return {
    EdgeType::CONTROLDEP_CALLINV,
    EdgeType::CONTROLDEP_CALLRET,
    EdgeType::CONTROLDEP_ENTRY,
    EdgeType::CONTROLDEP_BR,
    EdgeType::CONTROLDEP_IND_BR
  };
}

std::set<EdgeType> SlicingUtils::getParameterDependencyEdges()
{
  return {
    EdgeType::PARAMETER_IN,
    EdgeType::PARAMETER_OUT,
    EdgeType::PARAMETER_FIELD
  };
}

void SlicingUtils::printSlice(const NodeSet &slice, const std::string &slice_name)
{
  errs() << "=============== " << slice_name << " ===============\n";
  errs() << "Slice size: " << slice.size() << " nodes\n";
  
  for (auto *node : slice) {
    if (node == nullptr)
      continue;
      
    std::string str;
    raw_string_ostream OS(str);
    Value* val = node->getValue();
    if (val != nullptr) {
      if (Function* f = dyn_cast<Function>(val)) {
        OS << f->getName().str();
      } else if (Instruction* inst = dyn_cast<Instruction>(val)) {
        // Safely print instruction with error handling
        try {
          OS << *inst;
        } catch (...) {
          OS << "<invalid instruction>";
        }
      } else if (GlobalVariable* gv = dyn_cast<GlobalVariable>(val)) {
        OS << gv->getName().str();
      } else {
        // For other types of values, try to print safely
        try {
          OS << *val;
        } catch (...) {
          OS << "<invalid value>";
        }
      }
    }
    
    if (node->getValue() != nullptr)
      errs() << "node: " << node << " - " << pdgutils::rtrim(OS.str()) << " - " << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
    else
      errs() << "node: " << node << " - " << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
  }
  errs() << "==========================================\n";
}

std::unordered_map<std::string, size_t> SlicingUtils::getSliceStatistics(const NodeSet &slice)
{
  std::unordered_map<std::string, size_t> stats;
  stats["total_nodes"] = slice.size();
  
  std::unordered_map<GraphNodeType, size_t> node_type_counts;
  std::unordered_map<EdgeType, size_t> edge_type_counts;
  
  for (auto *node : slice) {
    if (node == nullptr)
      continue;
    
    // Count node types
    node_type_counts[node->getNodeType()]++;
    
    // Count edge types (both incoming and outgoing)
    for (auto *edge : node->getInEdgeSet())
      edge_type_counts[edge->getEdgeType()]++;
    for (auto *edge : node->getOutEdgeSet())
      edge_type_counts[edge->getEdgeType()]++;
  }
  
  // Add node type statistics
  for (const auto &pair : node_type_counts)
    stats["node_type_" + pdgutils::getNodeTypeStr(pair.first)] = pair.second;
  
  // Add edge type statistics
  for (const auto &pair : edge_type_counts)
    stats["edge_type_" + pdgutils::getEdgeTypeStr(pair.first)] = pair.second;
  
  return stats;
}

} // namespace pdg
