#pragma once
#include "Checker/TPA/Analysis/TaintAnalysis.h"

namespace tpa::taint {
/// Compatibility facade for the original upstream tracking analysis.
class TrackingTaintAnalysis : public TaintAnalysis {
public:
  using TaintAnalysis::TaintAnalysis;
  std::pair<bool, ProgramPointSet>
  runOnDefUseModule(const DefUseModule &module) {
    bool clean = TaintAnalysis::runOnDefUseModule(module, true);
    return {clean, getLossSites()};
  }
};
} // namespace tpa::taint
