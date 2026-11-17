#pragma once

#include "Checker/TPA/Precision/IndexVector.h"
#include "Checker/TPA/Support/ProgramPoint.h"

#include <vector>

namespace tpa {
class MemoryObject;
} // namespace tpa

namespace tpa::taint {

class TrackerGlobalState;
class TrackerWorkList;

class CallTracker {
private:
  TrackerGlobalState &trackerState;
  TrackerWorkList &workList;

  using CallerVector = std::vector<ProgramPoint>;

  TaintVector getArgTaintValues(const CallerVector &, size_t);
  TaintVector getMemoryTaintValues(const CallerVector &,
                                   const tpa::MemoryObject *);

  void trackValue(const ProgramPoint &, const CallerVector &);
  void trackMemory(const ProgramPoint &, const CallerVector &);

public:
  CallTracker(TrackerGlobalState &g, TrackerWorkList &w)
      : trackerState(g), workList(w) {}

  void trackCall(const ProgramPoint &, const CallerVector &);
};

} // namespace tpa::taint
