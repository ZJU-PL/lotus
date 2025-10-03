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
   * @brief Hash function for (Node*, call_stack) pairs used in visited set
   * 
   * This hash function combines the hash of the node pointer with the hash of
   * the call stack vector. It uses a combination of XOR and bit shifting to
   * create a good distribution of hash values for the visited set.
   */
  struct NodeStackHash {
    size_t operator()(const std::pair<Node *, std::vector<Node *>> &p) const {
      size_t hash = std::hash<Node *>{}(p.first);
      for (auto *node : p.second) {
        hash ^= std::hash<Node *>{}(node) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
      }
      return hash;
    }
  };

  /**
   * @brief Context-sensitive slicing using CFL-reachability
   * 
   * This class implements context-sensitive slicing using Context-Free Language (CFL)
   * reachability. It maintains a stack of call/return pairs to ensure that data flows
   * are tracked correctly across function boundaries, preventing spurious dependencies
   * from merging at function entry/exit points.
   */
  class ContextSensitiveSlicing
  {
  public:
    using NodeSet = std::set<Node *>;
    using VisitedSet = std::unordered_set<std::pair<Node *, std::vector<Node *>>, NodeStackHash>;
    
    /**
     * @brief Constructor
     * @param pdg Reference to the program dependency graph
     */
    explicit ContextSensitiveSlicing(GenericGraph &pdg) : _pdg(pdg) {}
    
    /**
     * @brief Compute context-sensitive forward slice from a single node
     * @param start_node The starting node for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive forward slice
     */
    NodeSet computeForwardSlice(Node &start_node, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute context-sensitive forward slice from multiple nodes
     * @param start_nodes Set of starting nodes for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive forward slice
     */
    NodeSet computeForwardSlice(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute context-sensitive backward slice from a single node
     * @param end_node The ending node for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive backward slice
     */
    NodeSet computeBackwardSlice(Node &end_node, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute context-sensitive backward slice from multiple nodes
     * @param end_nodes Set of ending nodes for the slice
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive backward slice
     */
    NodeSet computeBackwardSlice(const NodeSet &end_nodes, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Compute context-sensitive chop between source and sink nodes
     * @param source_node Source node
     * @param sink_node Sink node
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return Set of nodes in the context-sensitive chop
     */
    NodeSet computeChop(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types = {});
    
    /**
     * @brief Check if there exists a context-sensitive path from source to sink
     * @param source_node Source node
     * @param sink_node Sink node
     * @param edge_types Optional set of edge types to include (empty means all types)
     * @return True if a context-sensitive path exists
     */
    bool hasContextSensitivePath(Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types = {});
    
  private:
    GenericGraph &_pdg;
    
    /**
     * @brief Context-sensitive traversal with call stack (unified forward/backward)
     * @param start_nodes Starting nodes
     * @param edge_types Allowed edge types
     * @param forward True for forward traversal, false for backward
     * @return Set of reachable nodes
     */
    NodeSet traverseWithStack(const NodeSet &start_nodes, const std::set<EdgeType> &edge_types, bool forward);
    
  };

  /**
   * @brief Utility class for context-sensitive slicing operations
   */
  class ContextSensitiveSlicingUtils
  {
  public:
    using NodeSet = std::set<Node *>;
    
    /**
     * @brief Get all call/return edges for CFL-reachability
     * @return Set of call/return edge types
     */
    static std::set<EdgeType> getCallReturnEdges();
    
    /**
     * @brief Compare context-sensitive slice with context-insensitive slice
     * @param cs_slice Context-sensitive slice
     * @param ci_slice Context-insensitive slice
     * @return Statistics about the difference between slices
     */
    static std::unordered_map<std::string, size_t> compareSlices(const NodeSet &cs_slice, const NodeSet &ci_slice);
    
    /**
     * @brief Print context-sensitive slice information to stderr
     * @param slice Set of nodes in the slice
     * @param slice_name Name of the slice for identification
     */
    static void printContextSensitiveSlice(const NodeSet &slice, const std::string &slice_name);
    
    /**
     * @brief Get context-sensitive slice statistics
     * @param slice Set of nodes in the slice
     * @return Map of statistics (node types, edge types, context information, etc.)
     */
    static std::unordered_map<std::string, size_t> getContextSensitiveSliceStatistics(const NodeSet &slice);
    
    /**
     * @brief Get CFL-reachability statistics
     * @param slice Set of nodes in the slice
     * @return Map of CFL-specific statistics (call stack depths, matched pairs, etc.)
     */
    static std::unordered_map<std::string, size_t> getCFLReachabilityStatistics(const NodeSet &slice);
    
    /**
     * @brief Check if a path is CFL-valid (properly matched call/return pairs)
     * @param path Vector of nodes representing a path
     * @param pdg Reference to the program dependency graph
     * @return True if the path follows CFL-reachability constraints
     */
    static bool isCFLValidPath(const std::vector<Node *> &path, GenericGraph &pdg);
  };

} // namespace pdg
