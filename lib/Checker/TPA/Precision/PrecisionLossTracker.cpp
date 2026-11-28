#include "Checker/TPA/Precision/PrecisionLossTracker.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Checker/TPA/Engine/TaintGlobalState.h"
#include "Checker/TPA/Precision/LocalTracker.h"
#include "Checker/TPA/Precision/TrackerGlobalState.h"
#include "Checker/TPA/Precision/TrackerTransferFunction.h"
#include "Checker/TPA/Precision/TrackerWorkList.h"
#include "Checker/TPA/Program/DefUseModule.h"

#include <unordered_set>

#include <llvm/IR/Instructions.h>

namespace tpa::taint {

void PrecisionLossTracker::initializeWorkList(
    TrackerWorkList &workList, const SinkViolationRecord &record) {
  auto const &ptrAnalysis = globalState.getPointerAnalysis();
  for (auto const &mapping : record) {
    // We only care about imprecision
    auto const &pp = mapping.first;
    auto *cs = llvm::dyn_cast<llvm::CallBase>(
        pp.getDefUseInstruction()->getInstruction());
    assert(cs);
    const auto *ctx = pp.getContext();
    std::unordered_set<const llvm::Value *> trackedValues;
    std::unordered_set<const tpa::MemoryObject *> trackedObjects;

    for (auto const &violation : mapping.second) {
      if (violation.actualVal != TaintLattice::Either)
        continue;
      if (violation.argPos >= cs->arg_size())
        continue;

      auto *argVal = cs->getArgOperand(violation.argPos);
      if (violation.what == annotation::TClass::ValueOnly) {
        trackedValues.insert(argVal);
      } else if (violation.what == annotation::TClass::ReachableMemory) {
        auto pSet = ptrAnalysis.getPtsSet(ctx, argVal);
        if (pSet.empty())
          continue;
        auto const &memManager = ptrAnalysis.getMemoryManager();
        for (const auto *obj : pSet) {
          if (obj->isSpecialObject())
            continue;
          auto fields = memManager.getReachableMemoryObjects(obj);
          trackedObjects.insert(fields.begin(), fields.end());
        }
      } else {
        auto pSet = ptrAnalysis.getPtsSet(ctx, argVal);
        if (pSet.empty())
          continue;

        trackedObjects.insert(pSet.begin(), pSet.end());
      }
    }

    LocalTracker tracker(workList, globalState.getDefUseModule());
    tracker.trackValue(pp, trackedValues);
    tracker.trackMemory(pp, trackedObjects);
  }
}

ProgramPointSet
PrecisionLossTracker::trackImprecision(const SinkViolationRecord &record) {
  ProgramPointSet ppSet;

  // Prepare the tracker state
  TrackerGlobalState trackerState(
      globalState.getDefUseModule(), globalState.getPointerAnalysis(),
      globalState.getExternalTaintTable(), globalState.getEnv(),
      globalState.getMemo(), globalState.getCallGraph(), ppSet);

  // Prepare the tracker worklist
  TrackerWorkList workList;
  initializeWorkList(workList, record);

  // The main analysis
  while (!workList.empty()) {
    auto pp = workList.dequeue();

    TrackerTransferFunction(trackerState, workList).eval(pp);
  }

  return ppSet;
}

} // namespace tpa::taint