/**
 * @file QueryTypes.h
 * @brief Shared graph-query options, results, and traversal vocabulary.
 *
 * Service-specific types live with their query declarations.
 */

#pragma once

#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Core/QueryDiagnostics.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm {
class Value;
class Function;
class Module;
} // namespace llvm

namespace pdg {

class ProgramGraph;
class Node;
class Edge;

/// Named dependence-edge presets exposed to users of the query layer.
enum class PDGEdgePreset {
  All,
  Data,
  Control,
  Parameter,
  Interprocedural,
  ValueFlow,
  TransformLegality
};

/// Traversal mode for call/return matching.
enum class PDGContextMode { ContextInsensitive, ContextSensitive };

enum class PDGCachePolicy { Enabled, Disabled };

enum class SliceFlavor { Full, Thin };

enum class PDGWitnessPathKind {
  Slice,
  ShortestPath,
  AllShortestPath,
  Chop,
  TransformBlocker
};

/// Shared traversal guardrails. Zero means "unbounded".
struct PDGTraversalLimits {
  size_t max_depth = 0;
  size_t max_states = 0;
  size_t max_paths = 0;
  size_t max_path_length = 0;
  size_t max_stack_depth = 0;
};

struct PDGSourceLocation {
  std::string file;
  unsigned line = 0;
  unsigned column = 0;
};

/// A concrete explanatory path returned by a query.
struct PDGWitnessPath {
  PDGWitnessPathKind kind = PDGWitnessPathKind::Slice;
  std::vector<Node *> nodes;
  std::vector<EdgeType> edge_types;
  std::vector<Node *> call_stack;
};

/// Common carrier for graph-shaped query results.
struct PDGQueryResult {
  using NodeSet = std::set<Node *>;
  using EdgeSet = std::set<Edge *>;
  using PredecessorMap = std::unordered_map<Node *, std::set<Node *>>;
  using DistanceMap = std::unordered_map<Node *, size_t>;

  NodeSet criteria_nodes;
  NodeSet nodes;
  EdgeSet edges;
  PredecessorMap predecessors;
  DistanceMap distances;
  std::vector<PDGWitnessPath> witness_paths;
  PDGQueryDiagnostics diagnostics;

  bool empty() const { return nodes.empty() && edges.empty(); }
};

/// Scope restriction applied before or during traversal.
struct PDGQueryScope {
  enum class Kind { WholeGraph, NodeSet, Function, QueryResult };

  Kind kind = Kind::WholeGraph;
  PDGQueryResult::NodeSet nodes;
  const llvm::Function *function = nullptr;
  const PDGQueryResult *query_result = nullptr;

  static PDGQueryScope wholeGraph() { return PDGQueryScope{}; }

  static PDGQueryScope nodeSet(const PDGQueryResult::NodeSet &value) {
    PDGQueryScope scope;
    scope.kind = Kind::NodeSet;
    scope.nodes = value;
    return scope;
  }

  static PDGQueryScope functionScope(const llvm::Function &value) {
    PDGQueryScope scope;
    scope.kind = Kind::Function;
    scope.function = &value;
    return scope;
  }

  static PDGQueryScope queryResultScope(const PDGQueryResult &value) {
    PDGQueryScope scope;
    scope.kind = Kind::QueryResult;
    scope.query_result = &value;
    return scope;
  }
};

/// Execution options shared by all PDG query services.
struct PDGQueryOptions {
  PDGEdgePreset edge_preset = PDGEdgePreset::All;
  PDGQueryScope scope = PDGQueryScope::wholeGraph();
  PDGContextMode context_mode = PDGContextMode::ContextInsensitive;
  PDGTraversalLimits limits;
  PDGCachePolicy cache_policy = PDGCachePolicy::Enabled;
  bool explain = true;
  SliceFlavor slice_flavor = SliceFlavor::Full;
};

std::set<EdgeType> edgeTypesForPreset(PDGEdgePreset preset);

std::string describeNode(Node *node);

std::string stableNodeKey(Node *node);


} // namespace pdg
