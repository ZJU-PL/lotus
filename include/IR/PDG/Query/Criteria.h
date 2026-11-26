#pragma once

#include "IR/PDG/Query/PropertySpec.h"
#include "IR/PDG/Query/QueryTypes.h"

#include <utility>

namespace pdg {

/// Seed specification accepted by the query layer.
struct PDGCriteria {
  using NodeSet = PDGQueryResult::NodeSet;

  NodeSet nodes;
  std::vector<llvm::Value *> values;
  std::vector<std::string> function_names;
  std::vector<std::string> callee_names;
  std::vector<PDGSourceLocation> source_locations;
  std::vector<PropertySpec> property_specs;

  /// Preserve an explicitly supplied selection even if it matched no nodes.
  /// This differs from omitting criteria when a service defaults to all nodes.
  static PDGCriteria nodeSet(NodeSet selection) {
    PDGCriteria criteria;
    criteria.nodes = std::move(selection);
    criteria.has_node_selection_ = true;
    return criteria;
  }

  bool empty() const {
    return !has_node_selection_ && nodes.empty() && values.empty() &&
           function_names.empty() && callee_names.empty() &&
           source_locations.empty() && property_specs.empty();
  }

private:
  bool has_node_selection_ = false;
};

/// Materializes the nodes selected by a query scope.
PDGQueryResult::NodeSet nodesInScope(ProgramGraph &graph,
                                     const PDGQueryScope &scope);

/// Resolves high-level criteria into concrete PDG seed nodes.
class PDGCriteriaResolver {
public:
  explicit PDGCriteriaResolver(ProgramGraph &pdg) : pdg_(pdg) {}

  PDGQueryResult resolve(const PDGCriteria &criteria,
                         const PDGQueryOptions &options,
                         const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
};

} // namespace pdg
