#pragma once

#include "Checker/TPA/Precision/TrackerWorkList.h"
#include "Checker/TPA/Program/DefUseInstruction.h"
#include "Checker/TPA/Support/ProgramPoint.h"

namespace tpa::taint {

class LocalTracker {
private:
  TrackerWorkList &workList;

public:
  LocalTracker(TrackerWorkList &w) : workList(w) {}

  template <typename ValueSet>
  void trackValue(const ProgramPoint &pp, const ValueSet &vSet) {
    const auto *ctx = pp.getContext();
    const auto *duInst = pp.getDefUseInstruction();
    assert(!duInst->isEntryInstruction());

    // Enqueue all affected operands
    for (auto *predDuInst : duInst->top_preds()) {
      if (predDuInst->isEntryInstruction() ||
          vSet.count(predDuInst->getInstruction()))
        workList.enqueue(ProgramPoint(ctx, predDuInst));
    }
  }

  template <typename MemSet>
  void trackMemory(const ProgramPoint &pp, const MemSet &mSet) {
    const auto *ctx = pp.getContext();
    const auto *duInst = pp.getDefUseInstruction();
    assert(!duInst->isEntryInstruction());

    // Enqueue all affected memory predecessors in O(|mSet|).
    for (const auto *obj : mSet) {
      if (const auto *preds = duInst->findMemPreds(obj)) {
        for (auto *predDuInst : *preds)
          workList.enqueue(ProgramPoint(ctx, predDuInst));
      }
    }
  }
};

} // namespace tpa::taint
