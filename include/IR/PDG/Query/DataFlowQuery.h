/**
 * @file DataFlowQuery.h
 * @brief Dataflow-flavored PDG query services.
 */

#pragma once

#include "IR/PDG/Query/Criteria.h"

namespace pdg {

struct DefUseLink {
  Node *from = nullptr;
  Node *to = nullptr;
  EdgeType edge_type = EdgeType::DATA_DEF_USE;
};

struct ControllingCondition {
  Node *predicate = nullptr;
  EdgeType edge_type = EdgeType::CONTROLDEP_BR;
};

/// Dataflow-flavored queries built on top of PDG traversal.
class DataFlowQuery {
public:
  explicit DataFlowQuery(ProgramGraph &pdg) : pdg_(pdg) {}

  PDGQueryResult reachingDefinitions(
      const PDGCriteria &uses,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  std::vector<DefUseLink>
  defUseChain(Node &definition,
              const PDGQueryOptions &options = PDGQueryOptions()) const;

  std::vector<DefUseLink>
  useDefChain(Node &use,
              const PDGQueryOptions &options = PDGQueryOptions()) const;

  PDGQueryResult liveNodes(
      const PDGQueryOptions &options = PDGQueryOptions()) const;

  PDGQueryResult deadNodes(
      const PDGQueryOptions &options = PDGQueryOptions()) const;

  std::vector<ControllingCondition> immediateControllers(Node &node) const;

  PDGQueryResult allControllers(
      const PDGCriteria &criteria,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  PDGQueryResult controlRegion(
      const PDGCriteria &criteria,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
};

} // namespace pdg
