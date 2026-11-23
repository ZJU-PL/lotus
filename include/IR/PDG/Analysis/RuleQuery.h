#pragma once

#include "IR/PDG/Analysis/QueryCore.h"
#include "IR/PDG/Analysis/TaintQuery.h"

namespace pdg {

struct RuleDescriptor {
  std::string id;
  std::string severity;
  std::string codeql_query;
  /// Explicit coverage boundary, also included in machine-readable results.
  std::string coverage;
  /// Original CWE tags for CodeQL migrations, classifications for Lotus rules;
  /// membership does not imply complete CWE coverage.
  std::vector<unsigned> cwes;
};

struct RuleFinding {
  std::string rule_id;
  std::string message;
  Node *site = nullptr;
  std::vector<Node *> evidence;
  std::vector<TaintOrigin> taint_origins;
};

struct RuleQueryResult {
  std::vector<RuleDescriptor> rules;
  std::vector<RuleFinding> findings;
  PDGQueryDiagnostics diagnostics;
};

struct RuleQueryPolicy {
  /// Separate per-object lifetime budget; zero uses shared traversal limits.
  /// This does not lower the independent taint worklist budget.
  size_t lifetime_states_per_object = 0;
  /// Per-worker, per-path-check and per-cycle-search protocol budget.
  /// Zero disables this budget; shared query limits can still constrain it.
  size_t protocol_states_per_search = 4096;
};

/// Semantic checks over LLVM values associated with PDG nodes. Dependence
/// reachability alone is never interpreted as taint, must-flow, or a feasible
/// execution path. Works with both a full PDG and its structural graph.
class RuleQuery {
public:
  explicit RuleQuery(ProgramGraph &graph) : graph_(graph) {}
  static const std::vector<RuleDescriptor> &catalog();
  RuleQueryResult
  analyze(const std::vector<std::string> &rule_ids = {},
          const PDGCriteria &criteria = PDGCriteria(),
          const PDGQueryOptions &options = PDGQueryOptions(),
          const llvm::Module *module = nullptr,
          const TaintPolicy &taint_policy = TaintPolicy(),
          const RuleQueryPolicy &rule_policy = RuleQueryPolicy()) const;

private:
  ProgramGraph &graph_;
};

} // namespace pdg
