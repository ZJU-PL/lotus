#include "IR/PDG/Query/Criteria.h"

#include "llvm/IR/Module.h"

#include "IR/PDG/Query/QuerySupport.h"

namespace pdg {
using namespace llvm;
using namespace query_detail;

PDGQueryResult::NodeSet nodesInScope(ProgramGraph &graph,
                                     const PDGQueryScope &scope) {
  return scopeNodes(graph, scope);
}

PDGQueryResult PDGCriteriaResolver::resolve(const PDGCriteria &criteria,
                                            const PDGQueryOptions &options,
                                            const Module *module) const {
  PDGQueryResult result;

  for (NodeSet::const_iterator it = criteria.nodes.begin();
       it != criteria.nodes.end(); ++it) {
    if (*it != nullptr)
      result.nodes.insert(*it);
  }

  for (size_t i = 0; i < criteria.values.size(); ++i) {
    Value *value = criteria.values[i];
    if (value == nullptr)
      continue;
    if (Node *node = pdg_.getNode(*value))
      result.nodes.insert(node);
    else
      result.diagnostics.unresolved_criteria.push_back("value has no PDG node");
  }

  if (!criteria.function_names.empty()) {
    for (ProgramGraph::FuncWrapperMap::iterator it =
             pdg_.getFuncWrapperMap().begin();
         it != pdg_.getFuncWrapperMap().end(); ++it) {
      Function *function = it->first;
      if (function == nullptr)
        continue;
      const std::string function_name = toLower(function->getName().str());
      for (size_t name_index = 0; name_index < criteria.function_names.size();
           ++name_index) {
        if (function_name == toLower(criteria.function_names[name_index])) {
          if (Node *entry = pdg_.getNode(*function))
            result.nodes.insert(entry);
        }
      }
    }
  }

  if (!criteria.callee_names.empty()) {
    if (module == nullptr) {
      result.diagnostics.unresolved_criteria.push_back(
          "callee criteria requires module");
    } else {
      for (const Function &function : *module) {
        if (function.isDeclaration())
          continue;
        for (const BasicBlock &block : function) {
          for (const Instruction &inst : block) {
            const CallBase *call = dyn_cast<CallBase>(&inst);
            if (call == nullptr || call->getCalledFunction() == nullptr)
              continue;
            const std::string callee =
                toLower(call->getCalledFunction()->getName().str());
            for (size_t i = 0; i < criteria.callee_names.size(); ++i) {
              if (callee == toLower(criteria.callee_names[i])) {
                if (Node *node = pdg_.getNode(const_cast<Instruction &>(inst)))
                  result.nodes.insert(node);
              }
            }
          }
        }
      }
    }
  }

  if (!criteria.source_locations.empty()) {
    for (ProgramGraph::NodeSet::iterator it = pdg_.begin(); it != pdg_.end();
         ++it) {
      Node *node = *it;
      const Instruction *inst = dyn_cast_or_null<Instruction>(node->getValue());
      if (inst == nullptr)
        continue;
      for (size_t location_index = 0;
           location_index < criteria.source_locations.size();
           ++location_index) {
        if (sourceLocationMatches(criteria.source_locations[location_index],
                                  *inst)) {
          result.nodes.insert(node);
          break;
        }
      }
    }
  }

  if (!criteria.property_specs.empty()) {
    if (module == nullptr) {
      result.diagnostics.unresolved_criteria.push_back(
          "property criteria requires module");
    } else {
      for (size_t i = 0; i < criteria.property_specs.size(); ++i) {
        NodeSet property_nodes =
            resolvePropertyCriteria(pdg_, *module, criteria.property_specs[i]);
        result.nodes.insert(property_nodes.begin(), property_nodes.end());
      }
    }
  }

  const NodeSet scoped_nodes = scopeNodes(pdg_, options.scope);
  if (options.scope.kind != PDGQueryScope::Kind::WholeGraph) {
    NodeSet filtered;
    for (NodeSet::const_iterator it = result.nodes.begin();
         it != result.nodes.end(); ++it) {
      if (scoped_nodes.count(*it) != 0)
        filtered.insert(*it);
    }
    if (filtered.empty() && !result.nodes.empty())
      result.diagnostics.unresolved_criteria.push_back(
          "all resolved criteria were excluded by scope");
    result.nodes.swap(filtered);
  }

  result.criteria_nodes = result.nodes;
  return result;
}

} // namespace pdg
