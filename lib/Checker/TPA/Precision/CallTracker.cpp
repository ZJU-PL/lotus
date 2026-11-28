#include "Checker/TPA/Precision/CallTracker.h"

#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"
#include "Checker/TPA/Output/Printer.h"
#include "Checker/TPA/Precision/LocalTracker.h"
#include "Checker/TPA/Precision/TrackerGlobalState.h"
#include "Checker/TPA/Support/TaintEnv.h"
#include "Checker/TPA/Support/TaintMemo.h"

#include <cstddef>
#include <unordered_map>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace util::io;

namespace tpa::taint {

static const Value *getArgAtPos(const Instruction *inst, size_t idx) {
  auto *cs = llvm::dyn_cast<llvm::CallBase>(inst);
  assert(cs);
  if (idx >= cs->arg_size())
    return nullptr;

  return cs->getArgOperand(idx);
}

TaintVector CallTracker::getArgTaintValues(const CallerVector &callers,
                                           size_t idx) {
  TaintVector ret;
  ret.reserve(callers.size());

  for (auto const &callsite : callers) {
    const auto *arg =
        getArgAtPos(callsite.getDefUseInstruction()->getInstruction(), idx);
    auto taint =
        arg ? trackerState.getEnv().lookup(TaintValue(callsite.getContext(), arg))
            : TaintLattice::Unknown;
    ret.push_back(taint);
  }

  return ret;
}

void CallTracker::trackValue(const ProgramPoint &pp,
                             const CallerVector &callers) {
  const auto *callee = pp.getDefUseInstruction()->getFunction();
  auto numArgs = callee->arg_size();
  if (numArgs == 0)
    return;

  std::unordered_map<size_t, util::VectorSet<const llvm::Value *>>
      trackedValues;
  for (auto i = 0u; i < numArgs; ++i) {
    auto callTaints = getArgTaintValues(callers, i);

    auto demandingIndices = getDemandingIndices(callTaints);
    for (auto idx : demandingIndices)
      trackerState.addImprecisionSource(callers[idx]);

    auto impreciseIndices = getImpreciseIndices(callTaints);
    for (auto idx : impreciseIndices) {
      const auto *arg =
          getArgAtPos(callers[idx].getDefUseInstruction()->getInstruction(), i);
      if (arg)
        trackedValues[idx].insert(arg);
    }
  }

  LocalTracker localTracker(workList, trackerState.getDefUseModule());
  for (auto const &mapping : trackedValues)
    localTracker.trackValue(callers[mapping.first], mapping.second);
}

TaintVector CallTracker::getMemoryTaintValues(const CallerVector &callers,
                                              const tpa::MemoryObject *obj) {
  auto retVec = std::vector<TaintLattice>();
  retVec.reserve(callers.size());

  for (auto const &callsite : callers) {
    const auto *store = trackerState.getMemo().lookup(callsite);
    auto tVal = store ? store->lookup(obj) : TaintLattice::Unknown;
    retVec.push_back(tVal);
  }
  return retVec;
}

void CallTracker::trackMemory(const ProgramPoint &pp,
                              const CallerVector &callers) {
  const auto &memo = trackerState.getMemo();
  std::vector<size_t> activeCallers;
  std::vector<const TaintStore *> activeStores;
  activeCallers.reserve(callers.size());
  activeStores.reserve(callers.size());
  for (size_t i = 0, e = callers.size(); i < e; ++i) {
    const auto *store = memo.lookup(callers[i]);
    if (store && !store->empty()) {
      activeCallers.push_back(i);
      activeStores.push_back(store);
    }
  }
  if (activeStores.empty())
    return;

  // Bitwise OR across active caller stores identifies all objects whose joined
  // taint is Either (0b11) — the only objects that can produce demanding or
  // imprecise callers.
  llvm::DenseMap<uint32_t, uint64_t> unionChunks;
  for (const auto *store : activeStores)
    for (const auto &entry : store->chunks())
      unionChunks[entry.first] |= entry.second;

  const auto *duInst = pp.getDefUseInstruction();
  std::unordered_map<size_t, std::vector<const tpa::MemoryObject *>>
      trackedObjects;

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
        if (tVal == TaintLattice::Tainted)
          trackerState.addImprecisionSource(callers[activeCallers[k]]);
        else if (tVal == TaintLattice::Either)
          trackedObjects[activeCallers[k]].push_back(obj);
      }
    }
  }

  LocalTracker localTracker(workList, trackerState.getDefUseModule());
  for (auto const &mapping : trackedObjects)
    localTracker.trackMemory(callers[mapping.first], mapping.second);
}

void CallTracker::trackCall(const ProgramPoint &pp,
                            const std::vector<ProgramPoint> &callers) {
  if (!callers.empty()) {
    trackValue(pp, callers);
    trackMemory(pp, callers);
  }
}

} // namespace tpa::taint