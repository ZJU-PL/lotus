#include "Checker/TPA/Precision/ReturnTracker.h"

#include "Checker/TPA/Precision/TrackerGlobalState.h"
#include "Checker/TPA/Precision/TrackerWorkList.h"
#include "Checker/TPA/Precision/VectorTransform.h"
#include "Checker/TPA/Program/DefUseModule.h"
#include "Checker/TPA/Support/TaintEnv.h"
#include "Checker/TPA/Support/TaintMemo.h"

#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace tpa::taint {

ReturnTracker::ReturnVector
ReturnTracker::getReturnVector(const CalleeVector &callees, bool memory) {
  ReturnVector returns;
  for (const auto &fc : callees) {
    const auto &function =
        trackerState.getDefUseModule().getDefUseFunction(fc.getFunction());
    for (const auto *exit :
         (memory ? function.getMemoryExitInsts() : function.getExitInsts()))
      returns.emplace_back(fc.getContext(), exit);
  }
  return returns;
}

TaintVector ReturnTracker::getReturnTaintValues(const ReturnVector &retVec) {
  return vectorTransform(retVec, [this](auto const &returnSource) {
    auto retInst = returnSource.getDefUseInstruction()->getInstruction();
    auto retValue = cast<ReturnInst>(retInst)->getReturnValue();
    if (retValue == nullptr)
      return TaintLattice::Unknown;

    auto tVal = trackerState.getEnv().lookup(
        TaintValue(returnSource.getContext(), retValue));
    // assert(tVal != TaintLattice::Unknown);
    return tVal;
  });
}

TaintVector ReturnTracker::getReturnTaintValues(const tpa::MemoryObject *obj,
                                                const ReturnVector &retVec) {
  return vectorTransform(retVec, [this, obj](auto const &returnSource) {
    auto store = trackerState.getMemo().lookup(returnSource);
    auto tVal = store ? store->lookup(obj) : TaintLattice::Unknown;
    return tVal;
  });
}

void ReturnTracker::propagateReturns(const ReturnVector &retVec,
                                     const TaintVector &returnTaints) {
  assert(retVec.size() == returnTaints.size());

  auto indices = getImpreciseIndices(returnTaints);
  for (auto idx : indices)
    workList.enqueue(retVec[idx]);
}

void ReturnTracker::processReturns(const ProgramPoint &pp,
                                   const ReturnVector &retVec,
                                   const TaintVector &returnTaints) {
  if (!getDemandingIndices(returnTaints).empty())
    trackerState.addImprecisionSource(pp);

  propagateReturns(retVec, returnTaints);
}

void ReturnTracker::trackValue(const ProgramPoint &pp,
                               const ReturnVector &retVec) {
  const auto *inst = pp.getDefUseInstruction()->getInstruction();

  if (inst->getType()->isVoidTy())
    return;

  auto retTaints = getReturnTaintValues(retVec);

  processReturns(pp, retVec, retTaints);
}

void ReturnTracker::trackMemory(const ProgramPoint &pp,
                                const ReturnVector &retVec) {
  const auto &memo = trackerState.getMemo();
  std::vector<size_t> activeReturns;
  std::vector<const TaintStore *> activeStores;
  activeReturns.reserve(retVec.size());
  activeStores.reserve(retVec.size());
  for (size_t i = 0, e = retVec.size(); i < e; ++i) {
    const auto *store = memo.lookup(retVec[i]);
    if (store && !store->empty()) {
      activeReturns.push_back(i);
      activeStores.push_back(store);
    }
  }
  if (activeStores.empty())
    return;

  llvm::DenseMap<uint32_t, uint64_t> unionChunks;
  for (const auto *store : activeStores)
    for (const auto &entry : store->chunks())
      unionChunks[entry.first] |= entry.second;

  const auto *duInst = pp.getDefUseInstruction();
  bool markedSource = false;
  std::vector<uint8_t> enqueued(activeStores.size(), 0);
  size_t enqueuedCount = 0;

  for (const auto &entry : unionChunks) {
    const uint32_t baseId = entry.first << 5;
    const uint64_t w = entry.second;
    uint64_t eitherLanes = (w & (w >> 1)) & TaintStore::kOddLaneMask;
    while (eitherLanes) {
      const unsigned tz = static_cast<unsigned>(__builtin_ctzll(eitherLanes));
      const uint32_t objId = baseId | (tz >> 1);
      eitherLanes &= (eitherLanes - 1);
      const auto *obj = memo.getObjectById(objId);
      if (!duInst->findMemSuccs(obj))
        continue;
      for (size_t k = 0, ke = activeStores.size(); k < ke; ++k) {
        const auto tVal = activeStores[k]->lookupById(objId);
        if (tVal == TaintLattice::Tainted && !markedSource) {
          trackerState.addImprecisionSource(pp);
          markedSource = true;
        } else if (tVal == TaintLattice::Either && !enqueued[k]) {
          enqueued[k] = 1;
          ++enqueuedCount;
          workList.enqueue(retVec[activeReturns[k]]);
        }
      }
      if (markedSource && enqueuedCount == activeStores.size())
        return;
    }
  }
}

void ReturnTracker::trackReturn(const ProgramPoint &pp,
                                const CalleeVector &callees) {
  auto retVec = getReturnVector(callees);
  if (!retVec.empty())
    trackValue(pp, retVec);
  auto memoryExits = getReturnVector(callees, true);
  if (!memoryExits.empty())
    trackMemory(pp, memoryExits);
}

} // namespace tpa::taint