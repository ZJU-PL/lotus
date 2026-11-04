#pragma once

#include "Verification/TraceAbstraction/IcfgToNwa.h"

#include <map>

#include <z3++.h>

namespace lotus::verification::traceabstraction {
class TransitionFormulaBuilder;

enum class TraceStatus { Feasible, Infeasible, Unknown };

struct TraceCheckResult {
  TraceStatus status = TraceStatus::Unknown;
  // Declared first so expressions are destroyed before their context.
  std::shared_ptr<z3::context> context;
  std::vector<z3::expr> interpolants;
  std::map<std::string, std::string> assignments;
  std::string reason;
};

// Uses a canonical Int/LIA vocabulary shared by predicate generation and
// Hoare checking. Objects and Z3 expressions are confined to one thread.
class TraceChecker {
public:
  explicit TraceChecker(const ProgramAutomaton &program,
                        nwa::Context context = nwa::defaultContext());
  ~TraceChecker();
  TraceChecker(const TraceChecker &) = delete;
  TraceChecker &operator=(const TraceChecker &) = delete;
  TraceCheckResult check(const nwa::NestedRun &run);
  bool isInductive(const z3::expr &pre, nwa::Letter letter,
                   const z3::expr &post);
  bool isReturnInductive(const z3::expr &linearPre,
                         const z3::expr &hierarchicalPre, nwa::Letter letter,
                         const z3::expr &post);
  bool isTrue(const z3::expr &predicate);
  bool isFalse(const z3::expr &predicate);
  TransitionFormulaBuilder &semantics();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace lotus::verification::traceabstraction
