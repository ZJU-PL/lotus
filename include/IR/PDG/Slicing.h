/**
 * @file Slicing.h
 * @brief Header file for PDG slicing primitives
 *
 * This file defines the slicing primitives for the Program Dependency Graph (PDG):
 * - ForwardSlicing: Computes forward slices from slicing criteria
 * - BackwardSlicing: Computes backward slices to slicing criteria  
 * - ProgramChopping: Computes chops between source and sink nodes
 * - SlicingUtils: Utility functions for slice analysis and statistics
 *
 * These primitives enable various program analysis tasks such as impact analysis,
 * debugging, and program understanding by identifying relevant program dependencies.
 */

#pragma once
#include "IR/PDG/PDGNode.h"
#include "IR/PDG/PDGEdge.h"
#include "IR/PDG/PDGEnums.h"
#include "IR/PDG/Graph.h"
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

namespace pdg
{
  /**
   * @brief Forward slicing primitive for PDG
   * 
   * Forward slicing computes all nodes that can be reached from a given set of 
   * slicing criteria (starting nodes) by following the PDG edges in the forward direction.
   * This includes nodes that are data-dependent or control-dependent on the criteria.
   */
  class ForwardSlicing
  {
  public:
    using NodeSet = std::set<Node *>;
    using VisitedSet = std::unordered_set<Node *>;
    
    /**
     * @brief Constructor
     * @param pdg Reference to the program dependency graph
     */
    explicit ForwardSlicing(GenericGraph &pdg) : _pdg(pdg) {}
    
    /**
     * @brief Compute forward slice from a single node
     * @param start_node The starting node for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the forward slice
     */
    NodeSet computeSlice(Node &start_node, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute forward slice from multiple nodes
     * @param start_nodes Set of starting nodes for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the forward slice
     */
    NodeSet computeSlice(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute forward slice with depth limit
     * @param start_node The starting node for the slice
     * @param max_depth Maximum depth to traverse (0 means unlimited)
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the forward slice
     */
    NodeSet computeSliceWithDepth(Node &start_node, size_t max_depth, const std::set<EdgeType> &edge_types = {});
    
  private:
    GenericGraph &_pdg;
    
    /**
     * @brief Helper function to check if an edge type should be included
     * @param edge_type The edge type to check
     * @param allowed_types Set of allowed edge types (empty means all allowed)
     * @return True if the edge type should be included
     */
    bool isEdgeTypeAllowed(EdgeType edge_type, const std::set<EdgeType> &allowed_types) const;
  };

  /**
   * @brief Backward slicing primitive for PDG
   * 
   * Backward slicing computes all nodes that can reach a given set of slicing 
   * criteria (ending nodes) by following the PDG edges in the backward direction.
   * This includes nodes that the criteria are data-dependent or control-dependent on.
   */
  class BackwardSlicing
  {
  public:
    using NodeSet = std::set<Node *>;
    using VisitedSet = std::unordered_set<Node *>;
    
    /**
     * @brief Constructor
     * @param pdg Reference to the program dependency graph
     */
    explicit BackwardSlicing(GenericGraph &pdg) : _pdg(pdg) {}
    
    /**
     * @brief Compute backward slice from a single node
     * @param end_node The ending node for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the backward slice
     */
    NodeSet computeSlice(Node &end_node, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute backward slice from multiple nodes
     * @param end_nodes Set of ending nodes for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the backward slice
     */
    NodeSet computeSlice(const NodeSet &end_nodes, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute backward slice with depth limit
     * @param end_node The ending node for the slice
     * @param max_depth Maximum depth to traverse (0 means unlimited)
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the backward slice
     */
    NodeSet computeSliceWithDepth(Node &end_node, size_t max_depth, const std::set<EdgeType> &edge_types = {});
    
  private:
    GenericGraph &_pdg;
    
    /**
     * @brief Helper function to check if an edge type should be included
     * @param edge_type The edge type to check
     * @param allowed_types Set of allowed edge types (empty means all allowed)
     * @return True if the edge type should be included
     */
    bool isEdgeTypeAllowed(EdgeType edge_type, const std::set<EdgeType> &allowed_types) const;
  };

  /**
   * @brief Program chopping primitive for PDG
   * 
   * Program chopping computes all nodes that lie on paths between a set of source nodes
   * and a set of sink nodes. This is useful for computing the impact of certain program
   * points on other program points.
   */
  class ProgramChopping
  {
  public:
    using NodeSet = std::set<Node *>;
    using VisitedSet = std::unordered_set<Node *>;
    
    /**
     * @brief Constructor
     * @param pdg Reference to the program dependency graph
     */
    explicit ProgramChopping(GenericGraph &pdg) : _pdg(pdg) {}
    
    /**
     * @brief Compute program chop between source and sink nodes
     * @param source_nodes Set of source nodes
     * @param sink_nodes Set of sink nodes
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the chop
     */
    NodeSet computeChop(const NodeSet &source_nodes, const NodeSet &sink_nodes, 
                       const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute program chop between single source and sink nodes
     * @param source_node Single source node
     * @param sink_node Single sink node
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the chop
     */
    NodeSet computeChop(Node &source_node, Node &sink_node, 
                       const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute program chop with depth limit
     * @param source_nodes Set of source nodes
     * @param sink_nodes Set of sink nodes
     * @param max_depth Maximum depth to traverse (0 means unlimited)
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the chop
     */
    NodeSet computeChopWithDepth(const NodeSet &source_nodes, const NodeSet &sink_nodes,
                                size_t max_depth, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Check if there exists a path from source to sink
     * @param source_node Source node
     * @param sink_node Sink node
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return True if a path exists
     */
    bool hasPath(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Find all paths between source and sink nodes
     * @param source_node Source node
     * @param sink_node Sink node
     * @param max_paths Maximum number of paths to find (0 means unlimited)
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Vector of paths (each path is a vector of nodes)
     */
    std::vector<std::vector<Node *>> findAllPaths(Node &source_node, Node &sink_node,
                                                 size_t max_paths = 0, 
                                                 const std::set<EdgeType> &edge_types = {});
    
  private:
    GenericGraph &_pdg;
    
    /**
     * @brief Helper function to check if an edge type should be included
     * @param edge_type The edge type to check
     * @param allowed_types Set of allowed edge types (empty means all allowed)
     * @return True if the edge type should be included
     */
    bool isEdgeTypeAllowed(EdgeType edge_type, const std::set<EdgeType> &allowed_types) const;
    
    /**
     * @brief Helper function to find paths using DFS
     * @param current Current node in the path
     * @param sink Target sink node
     * @param visited Set of visited nodes in current path
     * @param current_path Current path being built
     * @param all_paths All paths found so far
     * @param max_paths Maximum number of paths to find
     * @param edge_types Set of allowed edge types
     */
    void findPathsDFS(Node &current, Node &sink, VisitedSet &visited,
                     std::vector<Node *> &current_path, std::vector<std::vector<Node *>> &all_paths,
                     size_t max_paths, const std::set<EdgeType> &edge_types);
  };


  /**
   * @brief Utility class for common slicing operations
   */
  class SlicingUtils
  {
  public:
    using NodeSet = std::set<Node *>;
    
    /**
     * @brief Get all data dependency edges
     * @return Set of data dependency edge types
     */
    static std::set<EdgeType> getDataDependencyEdges();
    
    /**
     * @brief Get all control dependency edges
     * @return Set of control dependency edge types
     */
    static std::set<EdgeType> getControlDependencyEdges();
    
    /**
     * @brief Get all parameter dependency edges
     * @return Set of parameter dependency edge types
     */
    static std::set<EdgeType> getParameterDependencyEdges();
    
    /**
     * @brief Print slice information to stderr
     * @param slice Set of nodes in the slice
     * @param slice_name Name of the slice for identification
     */
    static void printSlice(const NodeSet &slice, const std::string &slice_name);
    
    /**
     * @brief Get slice statistics
     * @param slice Set of nodes in the slice
     * @return Map of statistics (node types, edge types, etc.)
     */
    static std::unordered_map<std::string, size_t> getSliceStatistics(const NodeSet &slice);
  };

} // namespace pdg
