#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Checker/TPA/Annotation/ExternalTaintTable.h"
#include "Checker/TPA/Engine/TaintGlobalState.h"
#include "Checker/TPA/Engine/TransferFunction.h"
#include "Checker/TPA/Program/DefUseInstruction.h"
#include "Checker/TPA/Support/TaintEnv.h"
#include "Checker/TPA/Support/TaintValue.h"

#include <cstddef>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace annotation;
using namespace llvm;

namespace tpa::taint {

void TransferFunction::updateDirectMemoryTaint(const TaintValue &tv,
                                               TaintLattice taintVal,
                                               const ProgramPoint &pp,
                                               EvalResult &evalResult) {
  auto const &ptrAnalysis = globalState.getPointerAnalysis();
  auto pSet = ptrAnalysis.getPtsSet(tv.getContext(), tv.getValue());
  for (const auto *obj : pSet) {
    if (obj->isSpecialObject())
      continue;
    const bool first = !evalResult.hasOverride(obj);
    evalResult.weakUpdateOverride(obj, taintVal);
    if (first)
      evalResult.addDirectMemProp(pp, obj);
  }
}

void TransferFunction::updateReachableMemoryTaint(const TaintValue &tv,
                                                  TaintLattice taintVal,
                                                  const ProgramPoint &pp,
                                                  EvalResult &evalResult) {
  auto const &ptrAnalysis = globalState.getPointerAnalysis();
  auto const &memManager = ptrAnalysis.getMemoryManager();
  auto pSet = ptrAnalysis.getPtsSet(tv.getContext(), tv.getValue());
  for (const auto *obj : pSet) {
    if (obj->isSpecialObject())
      continue;

    auto dstObjs = memManager.getReachableMemoryObjects(obj);
    for (const auto *dstObj : dstObjs) {
      if (dstObj->isSpecialObject())
        continue;
      const bool first = !evalResult.hasOverride(dstObj);
      evalResult.weakUpdateOverride(dstObj, taintVal);
      if (first)
        evalResult.addDirectMemProp(pp, dstObj);
    }
  }
}

void TransferFunction::updateTaintValueByTClass(const TaintValue &tv,
                                                TClass taintClass,
                                                TaintLattice taintVal,
                                                const ProgramPoint &pp,
                                                EvalResult &evalResult) {
  switch (taintClass) {
  case TClass::ValueOnly: {
    auto envChanged = globalState.getEnv().strongUpdate(tv, taintVal);
    if (envChanged)
      addTopLevelSuccessors(pp, evalResult);
    break;
  }
  case TClass::DirectMemory: {
    updateDirectMemoryTaint(tv, taintVal, pp, evalResult);
    break;
  }
  case TClass::ReachableMemory: {
    updateReachableMemoryTaint(tv, taintVal, pp, evalResult);
    break;
  }
  }
}

void TransferFunction::updateTaintCallByTPosition(const ProgramPoint &pp,
                                                  TPosition taintPos,
                                                  TClass taintClass,
                                                  TaintLattice taintVal,
                                                  EvalResult &evalResult) {
  auto *cs = llvm::dyn_cast<llvm::CallBase>(
      pp.getDefUseInstruction()->getInstruction());
  assert(cs);

  if (taintPos.isReturnPosition()) {
    // Legacy LLVM intrinsic summaries can mention a nonexistent scalar return.
    if (cs->getType()->isVoidTy() && taintClass == TClass::ValueOnly)
      return;
    auto tv = TaintValue(pp.getContext(), cs);
    updateTaintValueByTClass(tv, taintClass, taintVal, pp, evalResult);
  } else {
    auto const &argPos = taintPos.getAsArgPosition();
    if (!argPos.isAfterArgPosition()) {
      if (argPos.getArgIndex() >= cs->arg_size())
        return;
      auto argTVal =
          TaintValue(pp.getContext(), cs->getArgOperand(argPos.getArgIndex()));
      updateTaintValueByTClass(argTVal, taintClass, taintVal, pp, evalResult);
    } else {
      for (size_t i = argPos.getArgIndex(), e = cs->arg_size(); i < e; ++i) {
        auto argTVal = TaintValue(pp.getContext(), cs->getArgOperand(i));
        updateTaintValueByTClass(argTVal, taintClass, taintVal, pp, evalResult);
      }
    }
  }
}

void TransferFunction::evalTaintSource(const ProgramPoint &pp,
                                       const SourceTaintEntry &entry,
                                       EvalResult &evalResult) {
  auto tPos = entry.getTaintPosition();
  auto tClass = entry.getTaintClass();

  updateTaintCallByTPosition(pp, tPos, tClass, entry.getTaintValue(),
                             evalResult);
}

TaintLattice TransferFunction::getTaintValueByTClass(const TaintValue &tv,
                                                     TClass tClass) {
  switch (tClass) {
  case TClass::ValueOnly:
    return globalState.getEnv().lookup(tv);
  case TClass::DirectMemory: {
    if (localState == nullptr)
      return TaintLattice::Unknown;
    // assert(localState != nullptr);

    auto const &ptrAnalysis = globalState.getPointerAnalysis();
    auto pSet = ptrAnalysis.getPtsSet(tv.getContext(), tv.getValue());
    if (pSet.empty())
      return TaintLattice::Unknown;
    TaintLattice retVal = loadTaintFromPtsSet(pSet, *localState);

    return retVal;
  }
  case TClass::ReachableMemory: {
    if (!localState)
      return TaintLattice::Unknown;
    const auto &pointer = globalState.getPointerAnalysis();
    auto result = TaintLattice::Unknown;
    for (auto *object : pointer.getPtsSet(tv.getContext(), tv.getValue())) {
      if (object->isUniversalObject())
        return TaintLattice::Either;
      if (object->isNullObject())
        continue;
      for (auto *field :
           pointer.getMemoryManager().getReachableMemoryObjects(object))
        result =
            Lattice<TaintLattice>::merge(result, localState->lookup(field));
    }
    return result;
  }
  }
}

void TransferFunction::evalMemcpy(const TaintValue &srcVal,
                                  const TaintValue &dstVal,
                                  const ProgramPoint &pp,
                                  EvalResult &evalResult) {
  if (localState == nullptr)
    return;
  // assert(localState != nullptr);

  auto const &ptrAnalysis = globalState.getPointerAnalysis();
  auto const &memManager = ptrAnalysis.getMemoryManager();

  auto dstSet = ptrAnalysis.getPtsSet(dstVal.getContext(), dstVal.getValue());
  auto srcSet = ptrAnalysis.getPtsSet(srcVal.getContext(), srcVal.getValue());
  if (dstSet.empty() || srcSet.empty())
    return;

  for (const auto *srcObj : srcSet) {
    if (srcObj->isSpecialObject())
      continue;

    auto srcObjs = memManager.getReachableMemoryObjects(srcObj);
    auto startingOffset = srcObj->getOffset();
    for (const auto *oObj : srcObjs) {
      auto oVal = TaintLattice::Unknown;
      if (oObj->isUniversalObject())
        oVal = TaintLattice::Either;
      else if (!oObj->isNullObject())
        oVal = localState->lookup(oObj);

      if (oVal == TaintLattice::Unknown || oObj->getOffset() < startingOffset)
        continue;

      auto offset = oObj->getOffset() - startingOffset;
      for (const auto *updateObj : dstSet) {
        const auto *tgtObj = memManager.offsetMemory(updateObj, offset);
        if (tgtObj->isSpecialObject())
          continue;
        const bool first = !evalResult.hasOverride(tgtObj);
        evalResult.weakUpdateOverride(tgtObj, oVal);
        if (first)
          evalResult.addDirectMemProp(pp, tgtObj);
      }
    }
  }
}

void TransferFunction::evalTaintPipe(const ProgramPoint &pp,
                                     const annotation::PipeTaintEntry &entry,
                                     EvalResult &evalResult) {
  auto *cs = llvm::dyn_cast<llvm::CallBase>(
      pp.getDefUseInstruction()->getInstruction());
  assert(cs);

  auto srcPos = entry.getSrcPosition();
  assert(!srcPos.isReturnPosition());

  auto dstPos = entry.getDstPosition();

  auto srcClass = entry.getSrcClass();
  auto dstClass = entry.getDstClass();

  if (srcClass == TClass::ReachableMemory &&
      dstClass == TClass::ReachableMemory && !dstPos.isReturnPosition() &&
      !dstPos.getAsArgPosition().isAfterArgPosition() &&
      !srcPos.getAsArgPosition().isAfterArgPosition()) {
    // This is the case of memcpy
    assert(!dstPos.isReturnPosition());
    assert(!dstPos.getAsArgPosition().isAfterArgPosition());

    auto srcIdx = srcPos.getAsArgPosition().getArgIndex();
    auto dstIdx = dstPos.getAsArgPosition().getArgIndex();
    if (srcIdx >= cs->arg_size() || dstIdx >= cs->arg_size())
      return;
    auto srcVal = TaintValue(pp.getContext(), cs->getArgOperand(srcIdx));
    auto dstVal = TaintValue(pp.getContext(), cs->getArgOperand(dstIdx));
    evalMemcpy(srcVal, dstVal, pp, evalResult);
  } else {
    const auto position = srcPos.getAsArgPosition();
    auto srcTaint = TaintLattice::Unknown;
    const auto end = position.isAfterArgPosition()
                         ? cs->arg_size()
                         : std::min<unsigned>(position.getArgIndex() + 1u,
                                              cs->arg_size());
    for (unsigned i = position.getArgIndex(); i < end; ++i) {
      const auto *argument = cs->getArgOperand(i);
      if (position.isAfterArgPosition() && srcClass != TClass::ValueOnly &&
          !argument->getType()->isPointerTy())
        continue;
      auto srcVal = TaintValue(pp.getContext(), argument);
      srcTaint = Lattice<TaintLattice>::merge(
          srcTaint, getTaintValueByTClass(srcVal, entry.getSrcClass()));
    }
    if (srcTaint == TaintLattice::Unknown)
      return;

    updateTaintCallByTPosition(pp, dstPos, dstClass, srcTaint, evalResult);
  }
}

void TransferFunction::evalCallBySummary(const ProgramPoint &pp,
                                         const Function *callee,
                                         const TaintSummary &summary,
                                         EvalResult &evalResult) {
  bool isSink = false;
  for (auto const &entry : summary) {
    switch (entry.getEntryEnd()) {
    case TEnd::Source:
      evalTaintSource(pp, entry.getAsSourceEntry(), evalResult);
      break;
    case TEnd::Pipe:
      evalTaintPipe(pp, entry.getAsPipeEntry(), evalResult);
      break;
    case TEnd::Sink:
      // We only care about taint source, but we need to record all taint sinks
      // for future examination
      isSink = true;
      break;
    }
  }

  if (isSink)
    globalState.getSinks().insert(SinkSignature(pp, callee));
}

void TransferFunction::evalExternalCall(const ProgramPoint &pp,
                                        const Function *func,
                                        EvalResult &evalResult) {
  if (isDebugIntrinsic(*func))
    return;
  auto funName = func->getName();
  const auto *summary = globalState.getExternalTaintTable().lookup(funName);
  if (summary && !summary->empty())
    return evalCallBySummary(pp, func, *summary, evalResult);
  if (!summary && globalState.getUnmodeledPolicy() == UnmodeledPolicy::Error)
    throw std::invalid_argument("Missing taint model for " + funName.str());

  evalIgnoredCall(pp, evalResult);
}

void TransferFunction::evalIgnoredCall(const ProgramPoint &pp,
                                       EvalResult &evalResult) {
  // Optimistic missing-model policy (also used for explicit IGNORE entries).
  // Existing memory/argument facts survive;
  // only this call's new SSA result is assumed untainted. Leaving it at bottom
  // would prevent downstream internal calls from being analyzed at all.
  const auto *inst = pp.getDefUseInstruction()->getInstruction();
  if (!inst->getType()->isVoidTy() &&
      globalState.getEnv().weakUpdate(TaintValue(pp.getContext(), inst),
                                      TaintLattice::Untainted))
    addTopLevelSuccessors(pp, evalResult);
  addMemBroadcast(pp, pp, evalResult);
}

} // namespace tpa::taint
