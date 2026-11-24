/**
 * @file SliceQuery.h
 * @brief Forward/backward slicing and chopping queries over the PDG.
 */

#pragma once

#include "IR/PDG/Query/Criteria.h"

namespace pdg {

/// Forward/backward slicing and chopping over the PDG.
class SliceQuery {
public:
  explicit SliceQuery(ProgramGraph &pdg);

  PDGQueryResult forward(const PDGCriteria &criteria,
                         const PDGQueryOptions &options = PDGQueryOptions(),
                         const llvm::Module *module = nullptr) const;

  PDGQueryResult backward(const PDGCriteria &criteria,
                          const PDGQueryOptions &options = PDGQueryOptions(),
                          const llvm::Module *module = nullptr) const;

  PDGQueryResult chop(const PDGCriteria &sources, const PDGCriteria &targets,
                      const PDGQueryOptions &options = PDGQueryOptions(),
                      const llvm::Module *module = nullptr) const;

private:
  ProgramGraph &pdg_;
  mutable std::unordered_map<std::string, PDGQueryResult> result_cache_;
  mutable std::unordered_map<std::string, PDGQueryResult::NodeSet>
      criteria_cache_;
  mutable unsigned long long cache_epoch_ = 0;
};

} // namespace pdg
