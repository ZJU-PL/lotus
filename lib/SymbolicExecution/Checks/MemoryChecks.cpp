//===----------------------------------------------------------------------===//
//
// Memory-safety bug query builders (BOF, NullDeref, Array OOB, Negative Index,
// Uninitialized Read, Use-After-Free, Double-Free).
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Checks/QueryEngine.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"
#include "SymbolicExecution/Models/MemoryAPI.h"
#include "SymbolicExecution/Support/AnalysisContext.h"
#include "SymbolicExecution/Support/AnalysisLimit.h"

#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <cassert>

using namespace llvm;
using namespace SymbolicExecution;

namespace {

StringRef simplifyIntrinsicName(StringRef IntrinsicName) {
  size_t StartIdx = IntrinsicName.find('.', 0);
  size_t EndIdx = IntrinsicName.find('.', StartIdx + 1);
  return IntrinsicName.substr(0, EndIdx);
}

} // namespace

bool QueryEngine::needsMemoryCheck(const PTItem &Pt) {
  if (Pt.isPlaceHolder()) {
    return false;
  }

  const auto &AllocSite = Pt.getAllocSite();
  if (Pt.isConcrete()) {
    if (AllocSite.isa<GuardedValueFlowNodeValue>() &&
        isa<AllocaInst>(AllocSite.getLLVMVal())) {
      auto *AllocI = cast<AllocaInst>(AllocSite.getLLVMVal());
      auto *AllocedTy = AllocI->getAllocatedType();
      if (AllocedTy->isArrayTy() || AllocI->isArrayAllocation()) {
        return true;
      }
      return AnalysisLimit::shouldAnalyzeNonArrayAlloca();
    }
    return true;
  }

  assert(Pt.isSymbolic());
  if (AllocSite.isa<GuardedValueFlowNodeValue>() &&
      isa<GlobalValue>(AllocSite.getLLVMVal())) {
    return false;
  }

  Type *AllocSiteTy = AllocSite.getType();
  return AllocSiteTy && AllocSiteTy->isPointerTy();
}

std::vector<NumericalQueryPtr>
QueryEngine::createBofQuery(const PTItem &Pt, const PropertyValuePtr &AccSz,
                            const GuardedProgramValSet &Deps) const {
  if (!needsMemoryCheck(Pt)) {
    return {};
  }

  auto Off = Pt.getOffset();
  if (Pt.isConcrete() && Off < int64_t(0)) {
    return {DirectNumericalQuery::createMustErr(BUG_TY_BOF)};
  }

  if (Pt.isConcrete()) {
    if (!AccSz) {
      return {DirectNumericalQuery::createTaintOnly(BUG_TY_BOF, Deps)};
    }

    auto ObjSz = Pt.getSize();
    auto Diff = Off + AccSz - ObjSz;
    if (Diff <= int64_t(0)) {
      return {};
    }

    if (IsaProperty<PropertyInteger>(Diff)) {
      return {DirectNumericalQuery::createMustErr(BUG_TY_BOF)};
    }

    if (IsaProperty<PropertySymExpr>(Off)) {
      auto MinusOff = -Off;
      auto Q1 = DirectNumericalQuery::createSymbolic(BUG_TY_BOF, Diff, false,
                                                     getDepsVals(Diff));
      auto Q2 = DirectNumericalQuery::createSymbolic(BUG_TY_BOF, MinusOff,
                                                     false,
                                                     getDepsVals(MinusOff));
      return {Q1, Q2};
    }

    return {DirectNumericalQuery::createSymbolic(BUG_TY_BOF, Diff, false,
                                                 getDepsVals(Diff))};
  }

  auto AccSzDeps = Deps;
  if (AccSz) {
    AccSzDeps = getDepsVals(AccSz);
  }
  return {IndirectNumericalQuery::createBof(
      Pt.getAllocSite(), Off, AccSz, getDepsVals(Off).merge(AccSzDeps))};
}

void QueryEngine::buildBofQueryLoadStore(Instruction *Inst,
                                         const ProgramValuePtr &Ptr,
                                         Type *AccTy,
                                         const AnalysisState &State) {
  if (!State.hasPts(Ptr)) {
    return;
  }

  const PtsSet &Pts = State.getPts(Ptr);
  auto AccSz =
      GetProperty<PropertyInteger>(State.getContext().getTypeSizeInBits(AccTy));

  Pts.forEach([&](const PTItem &Pt, const Condition &Cond) {
    auto Qs = createBofQuery(Pt, AccSz);
    for (const auto &Q : Qs) {
      addQueryTrace(
          Q, Cond,
          TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, Ptr.getLLVMVal()),
          State);
    }
  });
}

void QueryEngine::buildBofQueryLibCall(CallInst *Inst,
                                       const AnalysisState &State) {
  Function *Callee = Inst->getCalledFunction();
  if (!Callee || !Callee->isDeclaration()) {
    return;
  }

  StringRef FuncName = Callee->getName();
  if (Callee->isIntrinsic()) {
    FuncName = simplifyIntrinsicName(FuncName);
  }

  auto GenQueryForPtrBytes = [&](const ProgramValuePtr &Ptr,
                                 const GuardedSymbolicValSet &AccSzInBytes) {
    if (!State.hasPts(Ptr)) {
      return;
    }

    GuardedSymbolicValSet AccSizes;
    if (AccSzInBytes.empty()) {
      AccSizes.addValue(GetProperty<PropertyInteger>(0));
    } else {
      AccSizes = AccSzInBytes * PropertyInteger(8);
    }

    const auto &Pts = State.getPts(Ptr);
    Pts.forEach2(AccSizes, [&](const PTItem &Pt, const PropertyValuePtr &AccSz,
                               const Condition &Cond) {
      auto Qs = createBofQuery(Pt, AccSz);
      for (const auto &Q : Qs) {
        addQueryTrace(Q, Cond,
                      TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst,
                                Ptr.getLLVMVal()),
                      State);
      }
    });
  };

  auto GenQueryAccSizeDep = [&](const ProgramValuePtr &Ptr,
                                const ProgramValuePtr &Src) {
    if (State.hasPts(Ptr) && State.getTaintedVals().hasValue(Src)) {
      for (const auto &P : State.getPts(Ptr)) {
        auto Qs = createBofQuery(P.first, PropertyValuePtr(),
                                 GuardedProgramValSet(Src));
        for (const auto &Q : Qs) {
          addQueryTrace(Q, P.second,
                        TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst,
                                  Ptr.getLLVMVal()),
                        State);
        }
      }
    }
  };

  auto MatchLib = [Inst, &FuncName](const std::string &Target) {
    return isMatchLib(Inst, FuncName.str(), Target);
  };

  if (MatchLib("memset") || MatchLib("llvm.memset")) {
    GenQueryForPtrBytes(State.getNode(Inst->getArgOperand(0)),
                        State.getSymbolicVals(State.getNode(Inst->getArgOperand(2))));
  } else if (MatchLib("snprintf")) {
    Value *Ptr = Inst->getArgOperand(0);
    Value *NumBytes = Inst->getArgOperand(1);
    GenQueryForPtrBytes(State.getNode(Ptr),
                        State.getSymbolicVals(State.getNode(NumBytes)));
    GenQueryAccSizeDep(State.getNode(Ptr), State.getNode(Inst->getArgOperand(2)));
  } else if (MatchLib("memcpy") || MatchLib("memmove") ||
             MatchLib("llvm.memcpy") || MatchLib("llvm.memmove")) {
    const auto &Sizes =
        State.getSymbolicVals(State.getNode(Inst->getArgOperand(2)));
    GenQueryForPtrBytes(State.getNode(Inst->getArgOperand(0)), Sizes);
    GenQueryForPtrBytes(State.getNode(Inst->getArgOperand(1)), Sizes);
  } else if (MatchLib("fread") || MatchLib("fwrite")) {
    auto SzVals = State.getSymbolicVals(State.getNode(Inst->getArgOperand(1)));
    auto CountVals =
        State.getSymbolicVals(State.getNode(Inst->getArgOperand(2)));
    GenQueryForPtrBytes(State.getNode(Inst->getArgOperand(0)),
                        SzVals * CountVals);
  } else if (MatchLib("memchr")) {
    GenQueryForPtrBytes(State.getNode(Inst->getArgOperand(0)),
                        State.getSymbolicVals(State.getNode(Inst->getArgOperand(2))));
  } else if (MatchLib("strchr")) {
    GenQueryForPtrBytes(State.getNode(Inst->getOperand(0)), {});
  } else if (MatchLib("strcpy")) {
    Value *DstPtr = Inst->getOperand(0);
    Value *SrcPtr = Inst->getOperand(1);
    const auto &SrcLenVals = State.getStrlen(State.getNode(SrcPtr), Inst);
    GenQueryForPtrBytes(State.getNode(DstPtr), SrcLenVals + PropertyInteger(1));
    GenQueryForPtrBytes(State.getNode(SrcPtr), {});
    GenQueryAccSizeDep(State.getNode(DstPtr), State.getNode(SrcPtr));
  } else if (MatchLib("puts")) {
    Value *DstPtr = Inst->getOperand(0);
    const auto &LenVals = State.getStrlen(State.getNode(DstPtr), Inst);
    GenQueryForPtrBytes(State.getNode(DstPtr), LenVals + PropertyInteger(1));
  } else if (MatchLib("strncpy") || MatchLib("strncmp")) {
    const auto &Sizes =
        State.getSymbolicVals(State.getNode(Inst->getOperand(2)));
    GenQueryForPtrBytes(State.getNode(Inst->getOperand(0)), Sizes);
    GenQueryForPtrBytes(State.getNode(Inst->getOperand(1)), Sizes);
  } else if (MatchLib("strstr") || MatchLib("strcasestr") ||
             MatchLib("strcmp")) {
    GenQueryForPtrBytes(State.getNode(Inst->getOperand(0)), {});
    GenQueryForPtrBytes(State.getNode(Inst->getOperand(1)), {});
  } else if (MatchLib("strtok") || MatchLib("strlen")) {
    GenQueryForPtrBytes(State.getNode(Inst->getOperand(0)), {});
  } else if (MatchLib("strncat")) {
    Value *DstPtr = Inst->getOperand(0);
    Value *SrcPtr = Inst->getOperand(1);
    Value *NumBytes = Inst->getOperand(2);

    const auto &SrcLenVals = State.getStrlen(State.getNode(SrcPtr), Inst);
    const auto &DstLenVals = State.getStrlen(State.getNode(DstPtr), Inst);
    const auto &NLenVals = State.getSymbolicVals(State.getNode(NumBytes));

    GuardedSymbolicValSet AccSizes;
    for (const auto &SrcP : SrcLenVals) {
      for (const auto &DstP : DstLenVals) {
        for (const auto &NP : NLenVals) {
          PropertyValuePtr CopyLen = NP.first;
          auto Diff = NP.first - SrcP.first;
          if (Diff > int64_t(0)) {
            CopyLen = SrcP.first;
          }
          PropertyValuePtr AccSize = CopyLen + PropertyInteger(1) + DstP.first;
          AccSizes.addValue(AccSize, SrcP.second && DstP.second && NP.second);
        }
      }
    }

    GenQueryForPtrBytes(State.getNode(DstPtr), AccSizes);
    GenQueryForPtrBytes(State.getNode(SrcPtr), {});
  } else if (MatchLib("strcat")) {
    Value *DstPtr = Inst->getOperand(0);
    Value *SrcPtr = Inst->getOperand(1);
    const auto &SrcLenVals = State.getStrlen(State.getNode(SrcPtr), Inst);
    const auto &DstLenVals = State.getStrlen(State.getNode(DstPtr), Inst);
    GenQueryForPtrBytes(State.getNode(DstPtr),
                        SrcLenVals + DstLenVals + PropertyInteger(1));
    GenQueryForPtrBytes(State.getNode(SrcPtr), {});
    GenQueryAccSizeDep(State.getNode(DstPtr), State.getNode(SrcPtr));
  }
}

void QueryEngine::buildNullDerefQuery(Instruction *Inst,
                                      const AnalysisState &State) {
  Value *Ptr = nullptr;
  if (auto *LoadI = dyn_cast<LoadInst>(Inst)) {
    Ptr = getPointerOperand(LoadI);
  } else if (auto *StoreI = dyn_cast<StoreInst>(Inst)) {
    Ptr = getPointerOperand(StoreI);
  }
  if (!Ptr) {
    return;
  }

  auto PtrVals = State.getSymbolicVals(State.getNode(Ptr));
  PtrVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (Val == int64_t(0)) {
      addQueryTrace(DirectNumericalQuery::createMustErr(BUG_TY_NULL_DEREF),
                    Cond, TraceStep(TraceStep::TRACE_STEP_DEREF, Inst, Ptr),
                    State);
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      addQueryTrace(DirectNumericalQuery::createSymbolic(
                        BUG_TY_NULL_DEREF, Val, true, getDepsVals(Val)),
                    Cond, TraceStep(TraceStep::TRACE_STEP_DEREF, Inst, Ptr),
                    State);
    }
  });
}

void QueryEngine::buildArrayIndexOOBQuery(Instruction *Inst,
                                          const AnalysisState &State) {
  auto *GEP = dyn_cast<GetElementPtrInst>(Inst);
  if (!GEP) {
    return;
  }

  Value *BasePtr = GEP->getPointerOperand();
  auto *BaseNode = State.getNode(BasePtr);
  if (!State.hasPts(BaseNode)) {
    return;
  }

  const PtsSet &Pts = State.getPts(BaseNode);
  Pts.forEach([&](const PTItem &Pt, const Condition &Cond) {
    if (!needsMemoryCheck(Pt)) {
      return;
    }

    auto ObjSz = Pt.getSize();
    auto Off = Pt.getOffset();

    if (Pt.isConcrete()) {
      if (Off < int64_t(0)) {
        addQueryTrace(
            DirectNumericalQuery::createMustErr(BUG_TY_ARRAY_INDEX_OOB), Cond,
            TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, BasePtr),
            State);
        return;
      }

      if (ObjSz && IsaProperty<PropertyInteger>(Off) &&
          IsaProperty<PropertyInteger>(ObjSz) &&
          CastProperty<PropertyInteger>(Off)->getVal() >=
              CastProperty<PropertyInteger>(ObjSz)->getVal()) {
        addQueryTrace(
            DirectNumericalQuery::createMustErr(BUG_TY_ARRAY_INDEX_OOB), Cond,
            TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, BasePtr),
            State);
        return;
      }
    }

    if (IsaProperty<PropertySymExpr>(Off)) {
      PropertyValuePtr ZeroPtr = GetProperty<PropertyInteger>(0);
      auto NegQr = ZeroPtr - Off;
      addQueryTrace(DirectNumericalQuery::createSymbolic(
                        BUG_TY_ARRAY_INDEX_OOB, NegQr, false,
                        getDepsVals(NegQr)),
                    Cond,
                    TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, BasePtr),
                    State);

      if (ObjSz && IsaProperty<PropertyInteger>(ObjSz)) {
        auto OverflowQr = Off - ObjSz;
        addQueryTrace(DirectNumericalQuery::createSymbolic(
                          BUG_TY_ARRAY_INDEX_OOB, OverflowQr, false,
                          getDepsVals(OverflowQr)),
                      Cond,
                      TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst,
                                BasePtr),
                      State);
      }
    }
  });
}

void QueryEngine::buildNegativeArrayIndexQuery(Instruction *Inst,
                                               const AnalysisState &State) {
  auto *GEP = dyn_cast<GetElementPtrInst>(Inst);
  if (!GEP) {
    return;
  }

  for (unsigned I = 1; I < GEP->getNumOperands(); ++I) {
    Value *Idx = GEP->getOperand(I);
    auto IdxVals = State.getSymbolicVals(State.getNode(Idx));

    IdxVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
      if (Val < int64_t(0)) {
        addQueryTrace(
            DirectNumericalQuery::createMustErr(BUG_TY_NEGATIVE_ARRAY_INDEX),
            Cond, TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, Idx),
            State);
      } else if (IsaProperty<PropertySymExpr>(Val)) {
        PropertyValuePtr Zero(GetProperty<PropertyInteger>(0));
        auto NegIdx = Zero - Val;
        addQueryTrace(DirectNumericalQuery::createSymbolic(
                          BUG_TY_NEGATIVE_ARRAY_INDEX, NegIdx, false,
                          getDepsVals(Val)),
                      Cond,
                      TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, Idx),
                      State);
      }
    });
  }
}

void QueryEngine::buildUninitializedReadQuery(Instruction *Inst,
                                              const AnalysisState &State) {
  auto *LoadI = dyn_cast<LoadInst>(Inst);
  if (!LoadI) {
    return;
  }

  Value *Ptr = getPointerOperand(LoadI);
  auto *PtrNode = State.getNode(Ptr);
  if (!State.hasPts(PtrNode)) {
    return;
  }

  State.getPts(PtrNode).forEach([&](const PTItem &, const Condition &Cond) {
    addQueryTrace(DirectNumericalQuery::createMustErr(BUG_TY_UNINIT_READ), Cond,
                  TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, Ptr),
                  State);
  });
}

void QueryEngine::buildUafQuery(Instruction *Inst, const AnalysisState &State) {
  Value *Ptr = nullptr;
  if (auto *LoadI = dyn_cast<LoadInst>(Inst)) {
    Ptr = getPointerOperand(LoadI);
  } else if (auto *StoreI = dyn_cast<StoreInst>(Inst)) {
    Ptr = getPointerOperand(StoreI);
  }
  if (!Ptr) {
    return;
  }

  auto *PtrNode = State.getNode(Ptr);
  const auto &FreedPtrs = State.getFreedPtrs();

  if (FreedPtrs.hasValue(PtrNode)) {
    auto FreeCond = FreedPtrs.getGuardForValue(PtrNode);
    if (!FreeCond.isFalse()) {
      addQueryTrace(DirectNumericalQuery::createMustErr(BUG_TY_UAF), FreeCond,
                    TraceStep(TraceStep::TRACE_STEP_DEREF, Inst, Ptr), State);
    }
  }

  if (State.hasPts(PtrNode)) {
    State.getPts(PtrNode).forEach([&](const PTItem &Pt, const Condition &Cond) {
      auto AllocSite = Pt.getAllocSite();
      if (FreedPtrs.hasValue(AllocSite)) {
        auto CombinedCond = Cond && FreedPtrs.getGuardForValue(AllocSite);
        if (!CombinedCond.isFalse()) {
          addQueryTrace(DirectNumericalQuery::createMustErr(BUG_TY_UAF),
                        CombinedCond,
                        TraceStep(TraceStep::TRACE_STEP_DEREF, Inst, Ptr),
                        State);
        }
      }
    });
  }
}

void QueryEngine::buildDoubleFreeQuery(Instruction *Inst,
                                       const AnalysisState &State) {
  auto *CallI = dyn_cast<CallInst>(Inst);
  if (!CallI || CallI->arg_size() < 1) {
    return;
  }

  Function *Callee = getCallee(Inst);
  if (!isFreeLikeFunction(Callee)) {
    return;
  }

  Value *Ptr = CallI->getArgOperand(0);
  auto *PtrNode = State.getNode(Ptr);
  const auto &FreedPtrs = State.getFreedPtrs();

  bool HasDoubleFree = false;
  Condition DoubleFreeCond = Condition::getFalseCond();

  if (FreedPtrs.hasValue(PtrNode)) {
    HasDoubleFree = true;
    DoubleFreeCond = FreedPtrs.getGuardForValue(PtrNode);
  }

  if (State.hasPts(PtrNode)) {
    State.getPts(PtrNode).forEach([&](const PTItem &Pt, const Condition &Cond) {
      auto AllocSite = Pt.getAllocSite();
      if (!FreedPtrs.hasValue(AllocSite)) {
        return;
      }

      Condition CombinedCond = Cond && FreedPtrs.getGuardForValue(AllocSite);
      if (CombinedCond.isFalse()) {
        return;
      }

      if (HasDoubleFree) {
        DoubleFreeCond.orCond(CombinedCond);
      } else {
        HasDoubleFree = true;
        DoubleFreeCond = CombinedCond;
      }
    });
  }

  if (HasDoubleFree && !DoubleFreeCond.isFalse()) {
    addQueryTrace(DirectNumericalQuery::createMustErr(BUG_TY_DOUBLE_FREE),
                  DoubleFreeCond,
                  TraceStep(TraceStep::TRACE_STEP_CALL, Inst, Ptr), State);
  }
}
