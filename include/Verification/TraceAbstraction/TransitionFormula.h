#pragma once

#include "Verification/TraceAbstraction/IcfgToNwa.h"

#include <map>
#include <set>

#include <z3++.h>

namespace llvm {
class Value;
}
namespace lotus::verification::traceabstraction {

// Lotus counterpart of Ultimate's TransFormula: an immutable relation over
// canonical input, output, and (for returns) hierarchical input variables.
// The relation overapproximates unsupported/poisoned outcomes. A reported
// execution must also satisfy defined, using this SAME transition encoding.
struct TransitionFormula {
  z3::expr relation;
  z3::expr guard;
  z3::expr controlGuard;
  z3::expr defined;
  std::map<unsigned, z3::expr> assignments;
  std::set<unsigned> reads;
  std::set<unsigned> hierarchicalReads;
  std::set<unsigned> writes;
  std::set<unsigned> havoc;
};

struct StateVariable {
  const llvm::Value *value = nullptr;
  std::string name;
  bool shared = false; // heap/global state survives procedure returns
};

class TransitionFormulaBuilder {
public:
  explicit TransitionFormulaBuilder(
      const ProgramAutomaton &program,
      nwa::Context budget = nwa::defaultContext());
  ~TransitionFormulaBuilder();
  TransitionFormulaBuilder(const TransitionFormulaBuilder &) = delete;
  TransitionFormulaBuilder &
  operator=(const TransitionFormulaBuilder &) = delete;

  std::shared_ptr<z3::context> context() const;
  const z3::expr_vector &inputs() const;
  const z3::expr_vector &outputs() const;
  // Caller state at the matching call, not the state at the callee's exit.
  const z3::expr_vector &hierarchicalInputs() const;
  const std::vector<StateVariable> &variables() const;
  const TransitionFormula &build(nwa::Letter letter);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace lotus::verification::traceabstraction
