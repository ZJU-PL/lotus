#include "Checker/TPA/Engine/TaintPropagator.h"

#include "Checker/TPA/Engine/EvalResult.h"
#include "Checker/TPA/Engine/WorkList.h"
#include "Checker/TPA/Output/Printer.h"
#include "Checker/TPA/Program/DefUseInstruction.h"
#include "Checker/TPA/Support/TaintMemo.h"

#include <llvm/Support/raw_ostream.h>
using namespace util::io;

namespace tpa::taint {

void TaintPropagator::propagateObjFrom(const ProgramPoint &targetPP,
                                       const tpa::MemoryObject *obj,
                                       TaintLattice val) {
  if (val == TaintLattice::Unknown)
    return;
  const auto *succs = targetPP.getDefUseInstruction()->findMemSuccs(obj);
  if (!succs)
    return;
  const auto *ctx = targetPP.getContext();
  for (auto *succ : *succs) {
    ProgramPoint succPP(ctx, succ);
    if (memo.insert(succPP, obj, val))
      workList.enqueue(succPP);
  }
}

void TaintPropagator::propagate(
    const EvalResult &evalResult,
    llvm::ArrayRef<const tpa::MemoryObject *> dirtyObjs) {
  for (const auto &pp : evalResult.getTopSuccessors())
    workList.enqueue(pp);

  for (const auto &prop : evalResult.getDirectMemProps())
    propagateObjFrom(prop.targetPP, prop.obj, evalResult.lookup(prop.obj));

  const auto *base = evalResult.getBaseStore();
  const auto &overrides = evalResult.getOverrides();
  const size_t baseSize = base ? base->size() : 0;

  for (const auto &bc : evalResult.getMemBroadcasts()) {
    const auto *duInst = bc.targetPP.getDefUseInstruction();
    const size_t numSuccObjs = duInst->numMemSuccObjects();
    if (numSuccObjs == 0)
      continue;

    if (bc.full) {
      if (baseSize + overrides.size() <= numSuccObjs) {
        if (base) {
          for (const auto &entry : *base) {
            if (!overrides.empty() && overrides.findValue(entry.first))
              continue;
            propagateObjFrom(bc.targetPP, entry.first, entry.second);
          }
        }
        for (const auto &entry : overrides)
          propagateObjFrom(bc.targetPP, entry.first, entry.second);
      } else {
        const auto *ctx = bc.targetPP.getContext();
        duInst->forEachMemSucc(
            [&](const tpa::MemoryObject *loc, const auto &succs) {
              const TaintLattice val = evalResult.lookup(loc);
              if (val == TaintLattice::Unknown)
                return;
              for (auto *succ : succs) {
                ProgramPoint succPP(ctx, succ);
                if (memo.insert(succPP, loc, val))
                  workList.enqueue(succPP);
              }
            });
      }
    } else {
      for (const auto *obj : dirtyObjs)
        propagateObjFrom(bc.targetPP, obj, evalResult.lookup(obj));
      if (!overrides.empty()) {
        for (const auto &entry : overrides)
          propagateObjFrom(bc.targetPP, entry.first, entry.second);
      }
    }
  }
}

} // namespace tpa::taint