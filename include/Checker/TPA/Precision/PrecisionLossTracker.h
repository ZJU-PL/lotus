#pragma once

#include "Checker/TPA/Support/ProgramPointSet.h"
#include "Checker/TPA/Support/SinkViolationRecord.h"

#include <unordered_set>

namespace tpa::taint {

class TaintGlobalState;
class TrackerWorkList;

class PrecisionLossTracker {
private:
  const TaintGlobalState &globalState;

  void initializeWorkList(TrackerWorkList &, const SinkViolationRecord &);

public:
  PrecisionLossTracker(const TaintGlobalState &g) : globalState(g) {}

  ProgramPointSet trackImprecision(const SinkViolationRecord &);
};

} // namespace tpa::taint
