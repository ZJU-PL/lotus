#pragma once

#include "Checker/TPA/Precision/TrackerWorkList.h"
#include "Checker/TPA/Program/DefUseInstruction.h"
#include "Checker/TPA/Program/DefUseModule.h"
#include "Checker/TPA/Support/ProgramPoint.h"

#include <llvm/ADT/SmallPtrSet.h>

namespace tpa::taint {

class LocalTracker {
private:
  TrackerWorkList &workList;
  const DefUseModule &duModule;

public:
  LocalTracker(TrackerWorkList &w, const DefUseModule &m)
      : workList(w), duModule(m) {}

  template <typename ValueSet>
  void trackValue(const ProgramPoint &pp, const ValueSet &vSet) {
    if (vSet.empty())
      return;
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
    if (mSet.empty())
      return;
    const auto *ctx = pp.getContext();
    const auto *duInst = pp.getDefUseInstruction();
    assert(!duInst->isEntryInstruction());

    duModule.getDefUseFunction(duInst->getFunction()).ensureMemPreds();

    // Enqueue each distinct predecessor set at most once across equivalent
    // memory objects in the same group.
    llvm::SmallPtrSet<const util::VectorSet<DefUseInstruction *> *, 16>
        seenPredSets;
    for (const auto *obj : mSet) {
      if (const auto *preds = duInst->findMemPreds(obj)) {
        if (!seenPredSets.insert(preds).second)
          continue;
        for (auto *predDuInst : *preds)
          workList.enqueue(ProgramPoint(ctx, predDuInst));
      }
    }
  }
};

} // namespace tpa::taint
