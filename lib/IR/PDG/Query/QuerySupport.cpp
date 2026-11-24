#include "IR/PDG/Query/QuerySupport.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_set>

using namespace llvm;

namespace pdg::query_detail {

std::string toLower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string pointerKey(const void *value) {
  std::ostringstream os;
  os << value;
  return os.str();
}

static std::string locationKey(const DebugLoc &debug_loc) {
  if (!debug_loc)
    return "";
  std::string key = debug_loc->getFilename().str();
  key += ":";
  key += std::to_string(debug_loc.getLine());
  key += ":";
  key += std::to_string(debug_loc.getCol());
  return key;
}

bool sourceLocationMatches(const PDGSourceLocation &wanted,
                           const Instruction &inst) {
  DebugLoc loc = inst.getDebugLoc();
  if (!loc)
    return false;

  if (wanted.line != 0 && loc.getLine() != wanted.line)
    return false;
  if (wanted.column != 0 && loc.getCol() != wanted.column)
    return false;

  std::string file = loc->getFilename().str();
  if (wanted.file.empty())
    return true;
  if (file == wanted.file)
    return true;

  StringRef file_ref(file);
  StringRef wanted_ref(wanted.file);
  return file_ref.endswith(wanted_ref);
}

static NodeSet allGraphNodes(ProgramGraph &pdg) {
  NodeSet nodes;
  for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it)
    if (*it != nullptr)
      nodes.insert(*it);
  return nodes;
}

NodeSet scopeNodes(ProgramGraph &pdg, const PDGQueryScope &scope) {
  NodeSet nodes;
  if (scope.kind == PDGQueryScope::Kind::WholeGraph)
    return allGraphNodes(pdg);

  if (scope.kind == PDGQueryScope::Kind::NodeSet)
    return scope.nodes;

  if (scope.kind == PDGQueryScope::Kind::QueryResult && scope.query_result)
    return scope.query_result->nodes;

  if (scope.kind == PDGQueryScope::Kind::Function && scope.function) {
    for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end();
         ++it) {
      Node *node = *it;
      if (node == nullptr)
        continue;
      if (node->getFunc() == scope.function)
        nodes.insert(node);
    }
    if (pdg.hasNode(const_cast<Function &>(*scope.function)))
      nodes.insert(pdg.getNode(const_cast<Function &>(*scope.function)));
  }
  return nodes;
}

EdgeSet collectInducedEdges(const NodeSet &nodes,
                            const std::set<EdgeType> &edge_types) {
  EdgeSet edges;
  for (Node *node : nodes) {
    if (node == nullptr)
      continue;
    for (Node::EdgeSet::const_iterator it = node->getOutEdgeSet().begin();
         it != node->getOutEdgeSet().end(); ++it) {
      Edge *edge = *it;
      if (edge == nullptr)
        continue;
      if (!isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      if (nodes.count(edge->getDstNode()) == 0)
        continue;
      edges.insert(edge);
    }
  }
  return edges;
}

static std::string scopeCacheKey(const PDGQueryScope &scope) {
  std::ostringstream os;
  os << static_cast<int>(scope.kind) << ":";
  if (scope.kind == PDGQueryScope::Kind::Function && scope.function)
    os << scope.function->getName().str();
  if (scope.kind == PDGQueryScope::Kind::QueryResult && scope.query_result)
    os << pointerKey(scope.query_result);
  if (scope.kind == PDGQueryScope::Kind::NodeSet) {
    for (Node *node : scope.nodes)
      os << pointerKey(node) << ",";
  }
  return os.str();
}

std::string criteriaCacheKey(const PDGCriteria &criteria,
                             const llvm::Module *module) {
  std::ostringstream os;
  os << pointerKey(module) << "|";
  for (Node *node : criteria.nodes)
    os << "n:" << pointerKey(node) << ";";
  for (Value *value : criteria.values)
    os << "v:" << pointerKey(value) << ";";
  for (size_t i = 0; i < criteria.function_names.size(); ++i)
    os << "f:" << criteria.function_names[i] << ";";
  for (size_t i = 0; i < criteria.callee_names.size(); ++i)
    os << "c:" << criteria.callee_names[i] << ";";
  for (size_t i = 0; i < criteria.source_locations.size(); ++i) {
    os << "l:" << criteria.source_locations[i].file << ":"
       << criteria.source_locations[i].line << ":"
       << criteria.source_locations[i].column << ";";
  }
  for (size_t i = 0; i < criteria.property_specs.size(); ++i)
    os << "p:" << static_cast<int>(criteria.property_specs[i].getType()) << ":"
       << criteria.property_specs[i].rules().size() << ";";
  return os.str();
}

std::string optionsCacheKey(const PDGQueryOptions &options) {
  std::ostringstream os;
  os << static_cast<int>(options.edge_preset) << "|"
     << static_cast<int>(options.context_mode) << "|"
     << static_cast<int>(options.cache_policy) << "|"
     << static_cast<int>(options.slice_flavor) << "|" << options.explain << "|"
     << scopeCacheKey(options.scope) << "|" << options.limits.max_depth << "|"
     << options.limits.max_states << "|" << options.limits.max_paths << "|"
     << options.limits.max_path_length << "|" << options.limits.max_stack_depth;
  return os.str();
}

std::string pathKey(const PDGWitnessPath &path) {
  std::ostringstream os;
  os << static_cast<int>(path.kind) << "|";
  for (Node *node : path.nodes)
    os << pointerKey(node) << ",";
  return os.str();
}

std::string functionNameForNode(Node *node) {
  if (node == nullptr)
    return "";
  if (node->getFunc() != nullptr)
    return node->getFunc()->getName().str();
  const Function *function = dyn_cast_or_null<Function>(node->getValue());
  return function ? function->getName().str() : "";
}

std::string sourceKeyForNode(Node *node) {
  if (node == nullptr)
    return "";
  const Instruction *inst = dyn_cast_or_null<Instruction>(node->getValue());
  if (inst == nullptr || !inst->getDebugLoc())
    return "";
  return locationKey(inst->getDebugLoc());
}

std::string stringifyValue(const Value *value) {
  if (value == nullptr)
    return "";
  std::string rendered;
  raw_string_ostream os(rendered);
  os << *value;
  return os.str();
}

/// Resolve the callee behind pointer casts. Clang lowers calls to variadic
/// declarations such as @reach_error through an explicit bitcast, so
/// getCalledFunction() alone would miss reachability properties written in
/// terms of the original declaration name.
bool isEdgeAllowed(EdgeType type, const std::set<EdgeType> &allowed) {
  return allowed.empty() || allowed.count(type) != 0;
}

void syncCacheEpoch(
    ProgramGraph &pdg, unsigned long long &cache_epoch,
    std::unordered_map<std::string, PDGQueryResult> &result_cache,
    std::unordered_map<std::string, NodeSet> *criteria_cache,
    std::unordered_map<std::string,
                       std::unordered_map<Node *, std::set<Node *>>>
        *closure_cache) {
  if (cache_epoch == pdg.getEpoch())
    return;
  cache_epoch = pdg.getEpoch();
  result_cache.clear();
  if (criteria_cache != nullptr)
    criteria_cache->clear();
  if (closure_cache != nullptr)
    closure_cache->clear();
}

EdgeType edgeBetween(Node *from, Node *to) {
  if (from == nullptr || to == nullptr)
    return EdgeType::TYPE_OTHEREDGE;
  for (Node::EdgeSet::const_iterator it = from->getOutEdgeSet().begin();
       it != from->getOutEdgeSet().end(); ++it) {
    Edge *edge = *it;
    if (edge != nullptr && edge->getDstNode() == to)
      return edge->getEdgeType();
  }
  return EdgeType::TYPE_OTHEREDGE;
}

} // namespace pdg::query_detail
