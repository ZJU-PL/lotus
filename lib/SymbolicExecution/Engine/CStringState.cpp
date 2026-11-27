//===----------------------------------------------------------------------===//
//
// C-string length tracking state implementations.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Engine/CStringState.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Engine/AnalysisState.h"
#include "SymbolicExecution/Support/HashUtil.h"

using namespace llvm;
using namespace SymbolicExecution;

ProgramValuePtr CStringState::getLenVAtCaller(const ProgramValuePtr &LenV,
                                              Instruction *CS) {
  std::string Name = LenV.getID() + "_" + ptrToString(CS);
  return ProgramValuePtr(LenV.getType(), std::move(Name));
}

PropertyValuePtr CStringState::getAbsurdStrLen() {
  return GetProperty<PropertyInteger>(BigInteger(1000000));
}

void CStringState::handleCStrLen(const ProgramValuePtr &Ptr, Instruction *Loc,
                                 const Condition &Cond, AnalysisState &CurState,
                                 bool IsDirect) {
  ProgramValuePtr Len = getLenVariable(Ptr, Loc, CurState, IsDirect);
  GuardedSymbolicValSet LenValues;
  if (CurState.hasPts(Ptr)) {
    LenValues =
        computeCStrLength(Len, Loc, CurState.getPts(Ptr), CurState, Cond);
  } else if (CurState.mustBeConstantInt(Ptr)) {
    LenValues.addValue(getAbsurdStrLen());
  }

  if (LenValues.empty()) {
    LenValues.addValue(Len);
  }

  CurState.addSymbolicVals(Len, LenValues);
  CurState.addUnknownSym(Len);
}

GuardedSymbolicValSet
CStringState::getCStrlen(const ProgramValuePtr &Ptr, Instruction *Loc,
                         bool IsDirect, const AnalysisState &CurState) const {
  ProgramValuePtr Len = getLenVariable(Ptr, Loc, CurState, IsDirect);
  return CurState.getSymbolicVals(Len);
}

ProgramValuePtr CStringState::getLenVariable(const ProgramValuePtr &Ptr,
                                             Instruction *Loc,
                                             const AnalysisState &CurState,
                                             bool IsDirect) const {
  if (IsDirect) {
    return Loc ? ProgramValuePtr(CurState.getNode(Loc)) : ProgramValuePtr();
  }
  if (Ptr.isVacuous()) {
    return ProgramValuePtr();
  }
  std::string Name = Ptr.getID() + "_len_" + ptrToString(Loc);
  return ProgramValuePtr(CurState.getContext().getNonPtrType(),
                         std::move(Name));
}

GuardedSymbolicValSet
CStringState::computeCStrLength(const ProgramValuePtr &Len, Instruction *Loc,
                                const PtsSet &Pts, AnalysisState &CurState,
                                const Condition &Cond) {
  GuardedSymbolicValSet LenValues;
  (void)Cond;

  Pts.forEach(
      [&](const PTItem &Pt, const Condition &PtCond) {
        auto CurLenValues = computeCStrLength(Len, Loc, Pt, CurState, PtCond);
        LenValues.addValues(CurLenValues);
      },
      [&]() { return LenValues.isFull(); });

  return LenValues;
}

GuardedSymbolicValSet
CStringState::computeCStrLength(const ProgramValuePtr &Len, Instruction *Loc,
                                const PTItem &Pt, AnalysisState &CurState,
                                const Condition &Cond) {
  (void)Loc;
  (void)CurState;
  (void)Cond;
  // Conservative string length modeling until stored-zero byte queries are exposed.
  LenPts[Len].addValue(Pt);
  return {};
}

void CStringState::onProcessCall(Instruction *Inst, Function *Callee,
                                 AnalysisState &CurState,
                                 const CStringState &Smry) {
  const Condition &CSCond = CurState.getLocalCond(Inst->getParent());
  for (const auto &P : Smry.getLenPts()) {
    const auto &LenV = P.first;
    ProgramValuePtr LenVAtCaller = getLenVAtCaller(LenV, Inst);

    GuardedSymbolicValSet CallerLenValues;
    auto Res = CurState.inlineVals(P.second, Inst, Callee, CSCond);
    const auto &CallerPts = Res.first;
    bool Degenerate = Res.second;

    CallerPts.forEach(
        [&](const PTItem &Pt, const Condition &Cond) {
          auto CurLenValues =
              computeCStrLength(LenVAtCaller, Inst, Pt, CurState, Cond);
          CallerLenValues.addValues(CurLenValues);
        },
        [&]() { return CallerLenValues.isFull(); });

    if (CallerLenValues.empty()) {
      if (Degenerate) {
        CallerLenValues.addValue(getAbsurdStrLen());
      } else {
        CallerLenValues.addValue(LenVAtCaller);
      }
    }

    CurState.addSymbolicVals(LenVAtCaller, CallerLenValues);
    CurState.addUnknownSym(LenVAtCaller);
  }
}
