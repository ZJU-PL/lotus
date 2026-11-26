/**
 * @file DependenceQuery.h
 * @brief Reachability and path-oriented PDG query services.
 */

#pragma once

#include "IR/PDG/Query/Criteria.h"

namespace pdg {

/// Reachability and shortest-path style dependence queries.
class DependenceQuery {
public:
  explicit DependenceQuery(ProgramGraph &pdg);

  PDGQueryResult reachability(
      const PDGCriteria &sources,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  PDGQueryResult shortestPath(
      const PDGCriteria &sources, const PDGCriteria &targets,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  std::vector<PDGWitnessPath> allShortestPaths(
      const PDGCriteria &sources, const PDGCriteria &targets,
      const PDGQueryOptions &options = PDGQueryOptions(),
      const llvm::Module *module = nullptr) const;

  size_t distance(const PDGCriteria &sources, const PDGCriteria &targets,
                  const PDGQueryOptions &options = PDGQueryOptions(),
                  const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
  mutable std::unordered_map<std::string,
                             std::unordered_map<Node *, std::set<Node *>>>
      closure_cache_;
  mutable unsigned long long cache_epoch_ = 0;
};

} // namespace pdg
