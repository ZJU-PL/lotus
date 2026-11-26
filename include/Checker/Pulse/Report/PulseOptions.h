/** @file PulseOptions.h @brief Configuration options for Pulse analysis framework. */
#pragma once

namespace pulse {

namespace options {

/** If true, SMT solving is disabled (fast mode): path conditions are not
 * checked with Z3; unsatisfiability is not queried. Fewer paths may be pruned.
 */
inline bool &disableSMTRef() {
  static bool value = false;
  return value;
}

inline bool disableSMT() { return disableSMTRef(); }
inline void setDisableSMT(bool b) { disableSMTRef() = b; }

/** If true, malloc/calloc/realloc fork an additional OOM/null-return state.
 * Defaults to false for deterministic single-state allocation modeling unless
 * nullable allocation checking is explicitly enabled.
 */
inline bool &modelMallocFailureRef() {
  static bool value = false;
  return value;
}

inline bool modelMallocFailure() { return modelMallocFailureRef(); }
inline void setModelMallocFailure(bool b) { modelMallocFailureRef() = b; }

/** If true, latent issues are reported even when not discharged by a caller. */
inline bool &reportLatentIssuesRef() {
  static bool value = false;
  return value;
}

inline bool reportLatentIssues() { return reportLatentIssuesRef(); }
inline void setReportLatentIssues(bool b) { reportLatentIssuesRef() = b; }

/** If true, report unreachable heap memory / resource leaks at function exit. */
inline bool &enableMemoryLeaksRef() {
  static bool value = true;
  return value;
}

inline bool enableMemoryLeaks() { return enableMemoryLeaksRef(); }
inline void setEnableMemoryLeaks(bool b) { enableMemoryLeaksRef() = b; }

/** Tunable analysis budgets (aligned with Infer Pulse CLI options). */
inline unsigned &maxDisjunctsRef() {
  static unsigned value = 10u;
  return value;
}
inline unsigned maxDisjuncts() { return maxDisjunctsRef(); }
inline void setMaxDisjuncts(unsigned n) {
  maxDisjunctsRef() = (n == 0u ? 1u : n);
}

inline unsigned &maxLoopIterationsRef() {
  static unsigned value = 10u;
  return value;
}
inline unsigned maxLoopIterations() { return maxLoopIterationsRef(); }
inline void setMaxLoopIterations(unsigned n) {
  maxLoopIterationsRef() = (n == 0u ? 1u : n);
}

inline unsigned &widenThresholdRef() {
  static unsigned value = 2u;
  return value;
}
inline unsigned widenThreshold() { return widenThresholdRef(); }
inline void setWidenThreshold(unsigned n) {
  widenThresholdRef() = (n == 0u ? 1u : n);
}

} // namespace options
} // namespace pulse
