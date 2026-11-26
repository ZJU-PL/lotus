//===----------------------------------------------------------------------===//
//
// Arithmetic bug query builders (DBZ, integer overflow/underflow, shift
// overflow, integer truncation).
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Checks/QueryEngine.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"

#include <utility>

using namespace llvm;
using namespace SymbolicExecution;

namespace {

std::pair<BigInteger, BigInteger> getSignedMinMaxValues(Type *Ty) {
  unsigned BitWidth = Ty->getIntegerBitWidth();
  BigInteger One(1);
  APInt Shifted = One.getVal().shl(BitWidth - 1);
  BigInteger Min = -BigInteger(Shifted);
  BigInteger Max = BigInteger(Shifted) - One;
  return {Min, Max};
}

} // namespace

void QueryEngine::buildDbzQuery(Instruction *Inst, const AnalysisState &State) {
  const auto &DivisorVals =
      State.getSymbolicVals(State.getNode(Inst->getOperand(1)));
  DivisorVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (Val == int64_t(0)) {
      addQueryTrace(
          DirectNumericalQuery::createMustErr(BUG_TY_DBZ), Cond,
          TraceStep(TraceStep::TRACE_STEP_DIV, Inst, Inst->getOperand(1)),
          State);
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      addQueryTrace(DirectNumericalQuery::createSymbolic(BUG_TY_DBZ, Val, true,
                                                         getDepsVals(Val)),
                    Cond,
                    TraceStep(TraceStep::TRACE_STEP_DIV, Inst,
                              Inst->getOperand(1)),
                    State);
    }
  });
}

void QueryEngine::buildIntBoundQuery(Instruction *Inst, SymexBugType BugTy,
                                     bool CheckUpperBound,
                                     const AnalysisState &State) {
  if (!Inst->getType()->isIntegerTy()) {
    return;
  }

  auto Op1Vals = State.getSymbolicVals(State.getNode(Inst->getOperand(0)));
  auto Op2Vals = State.getSymbolicVals(State.getNode(Inst->getOperand(1)));

  GuardedSymbolicValSet ResVals;
  if (Inst->getOpcode() == Instruction::Add) {
    ResVals = Op1Vals + Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Sub) {
    ResVals = Op1Vals - Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Mul) {
    ResVals = Op1Vals * Op2Vals;
  } else {
    return;
  }

  auto MinMax = getSignedMinMaxValues(Inst->getType());
  PropertyInteger MaxVal(MinMax.second);
  PropertyValuePtr MinValPtr(GetProperty<PropertyInteger>(MinMax.first));

  ResVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (IsaProperty<PropertyInteger>(Val)) {
      auto IntVal = CastProperty<PropertyInteger>(Val)->getVal();
      bool OutOfRange =
          CheckUpperBound ? (IntVal > MinMax.second) : (IntVal < MinMax.first);
      if (OutOfRange) {
        addQueryTrace(DirectNumericalQuery::createMustErr(BugTy), Cond,
                      TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst),
                      State);
      }
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      PropertyValuePtr Qr = CheckUpperBound ? (Val - MaxVal) : (MinValPtr - Val);
      addQueryTrace(DirectNumericalQuery::createSymbolic(BugTy, Qr, false,
                                                         getDepsVals(Qr)),
                    Cond, TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst),
                    State);
    }
  });
}

void QueryEngine::buildShiftOverflowQuery(Instruction *Inst,
                                          const AnalysisState &State) {
  if (!Inst->getType()->isIntegerTy()) {
    return;
  }

  auto ShiftAmountVals =
      State.getSymbolicVals(State.getNode(Inst->getOperand(1)));
  unsigned BitWidth = Inst->getType()->getIntegerBitWidth();
  PropertyValuePtr MaxShift = GetProperty<PropertyInteger>(BitWidth);
  PropertyValuePtr Zero = GetProperty<PropertyInteger>(0);

  ShiftAmountVals.forEach(
      [&](const PropertyValuePtr &Val, const Condition &Cond) {
        if (IsaProperty<PropertyInteger>(Val)) {
          auto IntVal = CastProperty<PropertyInteger>(Val)->getVal();
          if (IntVal < 0 || IntVal >= BitWidth) {
            addQueryTrace(
                DirectNumericalQuery::createMustErr(BUG_TY_SHIFT_OVERFLOW),
                Cond, TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst),
                State);
          }
        } else if (IsaProperty<PropertySymExpr>(Val)) {
          auto NegQr = Zero - Val;
          auto OverflowQr = Val - MaxShift;
          addQueryTrace(DirectNumericalQuery::createSymbolic(
                            BUG_TY_SHIFT_OVERFLOW, NegQr, false,
                            getDepsVals(NegQr)),
                        Cond,
                        TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst),
                        State);
          addQueryTrace(DirectNumericalQuery::createSymbolic(
                            BUG_TY_SHIFT_OVERFLOW, OverflowQr, false,
                            getDepsVals(OverflowQr)),
                        Cond,
                        TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst),
                        State);
        }
      });
}

void QueryEngine::buildIntTruncationQuery(Instruction *Inst,
                                          const AnalysisState &State) {
  Value *Src = Inst->getOperand(0);
  Type *SrcTy = Src->getType();
  Type *DstTy = Inst->getType();
  if (!SrcTy->isIntegerTy() || !DstTy->isIntegerTy()) {
    return;
  }

  unsigned SrcBitWidth = SrcTy->getIntegerBitWidth();
  unsigned DstBitWidth = DstTy->getIntegerBitWidth();
  if (DstBitWidth >= SrcBitWidth) {
    return;
  }

  auto SrcVals = State.getSymbolicVals(State.getNode(Src));
  auto DstMinMax = getSignedMinMaxValues(DstTy);
  BigInteger DstMin = DstMinMax.first;
  BigInteger DstMax = DstMinMax.second;

  SrcVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (IsaProperty<PropertyInteger>(Val)) {
      BigInteger ValInt = CastProperty<PropertyInteger>(Val)->getAsBoundInt();
      if (ValInt < DstMin || ValInt > DstMax) {
        addQueryTrace(
            DirectNumericalQuery::createMustErr(BUG_TY_INT_TRUNCATION), Cond,
            TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Src), State);
      }
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      PropertyValuePtr MaxVal(
          GetProperty<PropertyInteger>(DstMax.getAsBoundInt()));
      PropertyValuePtr MinVal(
          GetProperty<PropertyInteger>(DstMin.getAsBoundInt()));

      auto UpperBound = Val - MaxVal;
      if (IsaProperty<PropertySymExpr>(UpperBound) ||
          (IsaProperty<PropertyInteger>(UpperBound) &&
           UpperBound > int64_t(0))) {
        addQueryTrace(DirectNumericalQuery::createSymbolic(
                          BUG_TY_INT_TRUNCATION, UpperBound, false,
                          getDepsVals(Val)),
                      Cond, TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Src),
                      State);
      }

      auto LowerBound = MinVal - Val;
      if (IsaProperty<PropertySymExpr>(LowerBound) ||
          (IsaProperty<PropertyInteger>(LowerBound) &&
           LowerBound > int64_t(0))) {
        addQueryTrace(DirectNumericalQuery::createSymbolic(
                          BUG_TY_INT_TRUNCATION, LowerBound, false,
                          getDepsVals(Val)),
                      Cond, TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Src),
                      State);
      }
    }
  });
}
