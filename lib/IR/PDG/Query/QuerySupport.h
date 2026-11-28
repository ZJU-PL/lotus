/// Internal query implementation helpers; use the focused query headers in clients.
#pragma once

#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Query/Criteria.h"

namespace pdg::query_detail {

using NodeSet = PDGQueryResult::NodeSet;
using EdgeSet = PDGQueryResult::EdgeSet;

std::string toLower(std::string value);
bool isEdgeAllowed(EdgeType type, const std::set<EdgeType> &allowed);
std::string pointerKey(const void *value);
bool sourceLocationMatches(const PDGSourceLocation &wanted,
                           const llvm::Instruction &inst);
NodeSet scopeNodes(ProgramGraph &pdg, const PDGQueryScope &scope);
EdgeSet collectInducedEdges(const NodeSet &nodes,
                            const std::set<EdgeType> &edge_types);
std::string criteriaCacheKey(const PDGCriteria &criteria,
                             const llvm::Module *module);
std::string optionsCacheKey(const PDGQueryOptions &options);
std::string pathKey(const PDGWitnessPath &path);
std::string functionNameForNode(Node *node);
std::string sourceKeyForNode(Node *node);
std::string stringifyValue(const llvm::Value *value);
std::string stringifyValue(const llvm::Value *value,
                           llvm::ModuleSlotTracker *slot_tracker);
NodeSet resolvePropertyCriteria(ProgramGraph &pdg, const llvm::Module &module,
                                const PropertySpec &spec);
void syncCacheEpoch(
    ProgramGraph &pdg, unsigned long long &cache_epoch,
    std::unordered_map<std::string, PDGQueryResult> &result_cache,
    std::unordered_map<std::string, NodeSet> *criteria_cache,
    std::unordered_map<std::string,
                       std::unordered_map<Node *, std::set<Node *>>>
        *closure_cache);
EdgeType edgeBetween(Node *from, Node *to);

} // namespace pdg::query_detail
