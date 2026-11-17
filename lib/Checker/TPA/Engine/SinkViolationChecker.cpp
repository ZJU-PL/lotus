#include "Checker/TPA/Engine/SinkViolationChecker.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Checker/TPA/Annotation/ExternalTaintTable.h"
#include "Checker/TPA/Program/DefUseInstruction.h"
#include "Checker/TPA/Support/SinkSignature.h"
#include "Checker/TPA/Support/TaintEnv.h"
#include "Checker/TPA/Support/TaintMemo.h"
#include "Checker/TPA/Support/TaintValue.h"

#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace annotation;
using namespace llvm;

namespace tpa::taint {

TaintLattice SinkViolationChecker::lookupTaint(const TaintValue &tv,
                                               TClass what,
                                               const TaintStore *store) {
  switch (what) {
  case TClass::ValueOnly:
    return env.lookup(tv);
  case TClass::DirectMemory: {
    if (!store)
      return TaintLattice::Unknown;
    auto pSet = ptrAnalysis.getPtsSet(tv.getContext(), tv.getValue());
    assert(!pSet.empty());
    auto res = TaintLattice::Unknown;
    for (const auto *loc : pSet) {
      auto val = store->lookup(loc);
      res = Lattice<TaintLattice>::merge(res, val);
    }
    return res;
  }
  case TClass::ReachableMemory:
    llvm_unreachable("ReachableMemory shouldn't appear in sink entry");
  }
}

void SinkViolationChecker::checkValueWithTClass(const TaintValue &tv,
                                                TClass tClass, unsigned argPos,
                                                const TaintStore *store,
                                                SinkViolationList &violations) {
  auto currVal = lookupTaint(tv, tClass, store);
  auto cmpRes =
      Lattice<TaintLattice>::compare(TaintLattice::Untainted, currVal);

  if (cmpRes != LatticeCompareResult::Equal &&
      cmpRes != LatticeCompareResult::GreaterThan)
    violations.push_back({argPos, tClass, TaintLattice::Untainted, currVal});
}

void SinkViolationChecker::checkCallSiteWithEntry(
    const ProgramPoint &pp, const SinkTaintEntry &entry,
    SinkViolationList &violations) {
  auto taintPos = entry.getArgPosition().getAsArgPosition();

  auto *cs = llvm::dyn_cast<llvm::CallBase>(
      pp.getDefUseInstruction()->getInstruction());
  auto checkArgument = [this, &pp, &entry, &violations, &cs](size_t idx) {
    const auto *store = memo.lookup(pp);

    auto argVal = TaintValue(pp.getContext(), cs->getArgOperand(idx));
    checkValueWithTClass(argVal, entry.getTaintClass(), idx, store, violations);
  };
  if (taintPos.isAfterArgPosition()) {
    for (size_t i = taintPos.getArgIndex(), e = cs->arg_size(); i < e; ++i)
      checkArgument(i);
  } else {
    checkArgument(taintPos.getArgIndex());
  }
}

SinkViolationList
SinkViolationChecker::checkCallSiteWithSummary(const ProgramPoint &pp,
                                               const TaintSummary &summary) {
  SinkViolationList violations;
  auto *cs = llvm::dyn_cast<llvm::CallBase>(
      pp.getDefUseInstruction()->getInstruction());

  for (auto const &entry : summary) {
    // We only care about the sink here
    if (entry.getEntryEnd() != TEnd::Sink)
      continue;

    checkCallSiteWithEntry(pp, entry.getAsSinkEntry(), violations);
  }

  return violations;
}

void SinkViolationChecker::checkSinkViolation(const SinkSignature &sig,
                                              SinkViolationRecord &records) {
  if (const auto *taintSummary = table.lookup(sig.getCallee()->getName())) {
    auto callsite = sig.getCallSite();
    auto violations =
        checkCallSiteWithSummary(sig.getCallSite(), *taintSummary);
    if (!violations.empty())
      records[callsite].insert(records[callsite].end(), violations.begin(),
                               violations.end());
  } else
    llvm_unreachable("Unrecognized external function call");
}

} // namespace tpa::taint