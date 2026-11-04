#pragma once

#include "Verification/TraceAbstraction/TraceChecker.h"

namespace lotus::verification::traceabstraction {
class InterpolantAutomatonBuilder {
public:
  // Validates every certificate edge, including the supplied spine, before
  // it can participate in language subtraction. Returns nullptr on failure.
  // Successors are generalized on demand. The checker must outlive the
  // returned provider, which retains its own predicate/context references.
  static nwa::Provider build(const ProgramAutomaton &program,
                             const nwa::NestedRun &run,
                             const TraceCheckResult &proof,
                             TraceChecker &checker, bool generalize = true,
                             nwa::Context context = nwa::defaultContext());
};
} // namespace lotus::verification::traceabstraction
