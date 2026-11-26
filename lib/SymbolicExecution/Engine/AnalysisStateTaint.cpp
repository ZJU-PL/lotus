//===----------------------------------------------------------------------===//
//
// AnalysisState taint analysis implementation.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Engine/AnalysisState.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"

#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "Symex"

using namespace llvm;
using namespace SymbolicExecution;

void AnalysisState::taintInit(Function *Func) {
  if (Func->getName() == "main") {
    auto &EntryBB = Func->getEntryBlock();
    if (EntryBB.empty()) {
      return;
    }

    auto ArgList = Func->args();
    if (Func->arg_size() >= 2) {
      auto *Iter = ArgList.begin();
      ++Iter;

      Argument *Arg = &*Iter;
      if (Arg->getType()->isPointerTy() &&
          Arg->getType()->getPointerElementType()->isPointerTy() &&
          Arg->getType()
              ->getPointerElementType()
              ->getPointerElementType()
              ->isIntegerTy(8)) {
        taintVal(getNode(Arg), {}, Condition());
      }
    }
  }
}

void AnalysisState::taintTransfer(Instruction *Inst) {
  if (Inst->getOpcode() == Instruction::Call) {
    if (!isDefiniteCall(Inst)) {
      processCallTaintSources(Inst);
      processTaintPropagation(Inst);
    }
  } else {
    processTaintPropagation(Inst);
  }
}

void AnalysisState::processCallTaintSources(Instruction *Inst) {
  Function *CalleeFunc = getCallee(Inst);
  if (!CalleeFunc || !CalleeFunc->isDeclaration()) {
    return;
  }

  std::set<Var> TaintedDsts = TaintSpec.getTaintedVars(Inst, Graph);
  if (TaintedDsts.empty()) {
    return;
  }

  LLVM_DEBUG(dbgs() << "[DEBUG-Numerical] Found Taint source in func "
                    << F->getName() << ":\n";
             Inst->print(dbgs(), true); dbgs() << "\n";);

  Condition BBCond = getLocalCond(Inst->getParent());
  for (const auto &V : TaintedDsts) {
    auto *SrcVal = V.getValue().getLLVMVal();
    assert(SrcVal);
    TaintStep SrcStep(TaintStep::TAINT_STEP_SOURCE, Inst, SrcVal);
    taintVal(V.getValue(), {SrcStep}, BBCond);
  }
}

void AnalysisState::taintVal(const ProgramValuePtr &V,
                             const std::vector<TaintStep> &Steps,
                             const Condition &PreCond, bool Peel) {
  markTaint(V, Steps, PreCond);

  if (!Peel || !hasSymbolicVals(V)) {
    return;
  }

  const auto &Vals = getSymbolicVals(V);
  for (const auto &P : Vals) {
    const auto &Val = P.first;
    const auto &Cond = P.second;
    if (IsaProperty<PropertySymExpr>(Val)) {
      const auto *SymVal = CastProperty<PropertySymExpr>(Val);

      bool ShouldPropagate = false;
      if (V.getType()->isIntegerTy()) {
        ShouldPropagate = (SymVal->getUsedVars().size() == 1);
      } else if (V.getType()->isPointerTy()) {
        ShouldPropagate = (SymVal->getUsedVars().size() > 1) || SymVal->isVar();
      }

      if (!ShouldPropagate) {
        continue;
      }

      for (const auto &DstV : SymVal->getUsedVars()) {
        if (V.getType()->isIntegerTy() ||
            DstV.getValue().getType()->isPointerTy()) {
          markTaint(DstV.getValue(), Steps, Cond && PreCond);
        }
      }
    }
  }
}

void AnalysisState::markTaint(const ProgramValuePtr &V,
                              const std::vector<TaintStep> &Steps,
                              const Condition &Cond) {
  if (V.isConstant()) {
    return;
  }

  if (!TaintedSteps.count(V)) {
    TaintedSteps[V] = Steps;
  }

  TaintedVals.addValue(V, Cond);

  LLVM_DEBUG(dbgs() << "[DEBUG-Numerical] propagate taint in func "
                    << F->getName() << ":\n";
             V.dump(););
}

void AnalysisState::processTaintPropagation(Instruction *Inst) {
  if (isa<CmpInst>(Inst)) {
    return;
  }

  Condition BBCond;
  if (&*Inst->getParent()->begin() == Inst) {
    BBCond = getLocalCond(Inst->getParent());
  }

  if (Inst->getOpcode() == Instruction::Call) {
    auto TaintedTransfers = getTaintTransferTargets(Inst);
    for (const auto &P : TaintedTransfers) {
      const auto &Src = P.first;
      for (const auto &Dst : P.second) {
        propagateTaint(Src, Dst, Inst, BBCond);
      }
    }
    return;
  }

  if (Inst->getType()->isVoidTy()) {
    return;
  }

  ProgramValuePtr DstV(getNode(Inst));
  bool IsDstPointer = DstV.getType()->isPointerTy();

  if (isInstUnmodelled(Inst)) {
    for (unsigned Idx = 0; Idx < Inst->getNumOperands(); ++Idx) {
      auto OpndVals = getSymbolicVals(getNode(Inst->getOperand(Idx)));
      for (const auto &P : OpndVals) {
        const auto &OpndVal = P.first;
        if (IsaProperty<PropertySymExpr>(OpndVal)) {
          for (const auto &DepV :
               CastProperty<PropertySymExpr>(OpndVal)->getUsedVars()) {
            propagateTaint(DepV.getValue(), DstV, Inst, P.second && BBCond);
          }
        }
      }
    }
    return;
  }

  for (const auto &P : getSymbolicVals(DstV)) {
    const auto &SymVal = P.first;
    if (IsaProperty<PropertyInteger>(SymVal))
      continue;
    Condition PreCond = BBCond && P.second;
    for (const auto &Sym :
         CastProperty<PropertySymExpr>(SymVal)->getUsedVars()) {
      auto SrcV = Sym.getValue();
      bool ValuePassing = !IsDstPointer || SrcV.getType()->isPointerTy();
      if (ValuePassing) {
        propagateTaint(SrcV, DstV, Inst, PreCond);
      }
    }
  }
}

void AnalysisState::propagateTaint(const ProgramValuePtr &Src,
                                   const ProgramValuePtr &Dst,
                                   Instruction *Inst, const Condition &Cond,
                                   bool Peel) {
  Condition PreCond = getTaintedCond(Src) && Cond;
  if (PreCond.isFalse()) {
    return;
  }

  std::vector<TaintStep> Steps;
  if (TaintedSteps.count(Src)) {
    Steps = TaintedSteps.at(Src);
  }

  if (Dst.isa<GuardedValueFlowNodeValue>() &&
      Src.isa<GuardedValueFlowNodeValue>() && Inst) {
    Steps.emplace_back(TaintStep::TAINT_STEP_PROP, Inst, Src.getLLVMVal(),
                       Dst.getLLVMVal());
  }

  taintVal(Dst, Steps, PreCond, Peel);
}

std::unordered_map<ProgramValuePtr, std::unordered_set<ProgramValuePtr>>
AnalysisState::getTaintTransferTargets(Instruction *Inst) const {
  std::unordered_map<ProgramValuePtr, std::unordered_set<ProgramValuePtr>> Res;
  auto *CS = dyn_cast<CallBase>(Inst);
  if (!CS) {
    return Res;
  }

  assert(!isDefiniteCall(Inst));
  for (size_t Idx = 0; Idx < CS->arg_size(); ++Idx) {
    Value *ArgVal = CS->getArgOperand(Idx);
    if (!ArgVal) {
      continue;
    }

    ArgVal = ArgVal->stripPointerCasts();
    auto *ArgNode = Graph->findNode(ArgVal);
    if (!ArgNode) {
      continue;
    }

    ProgramValuePtr Arg(ArgNode);
    std::vector<Value *> DstVect;
    TaintSpec.getTransferDstVect(CS, ArgNode->getLLVMValue(), DstVect);

    for (auto *V : DstVect) {
      if (!V) {
        continue;
      }

      V = V->stripPointerCasts();
      if (auto *DstNode = Graph->findNode(V)) {
        Res[Arg].insert(ProgramValuePtr(DstNode));
      }
    }
  }

  if (!Res.empty()) {
    return Res;
  }

  Function *Callee = getCallee(Inst);
  if (Callee && Callee->isIntrinsic()) {
    switch (Callee->getIntrinsicID()) {
    case Intrinsic::memset:
    case Intrinsic::memmove:
    case Intrinsic::memcpy:
      Res[getNode(CS->getArgOperand(1))].insert(getNode(CS->getArgOperand(0)));
      break;
    default:
      break;
    }
  }

  return Res;
}

void AnalysisState::propagateTaintPointer(const ProgramValuePtr &LdPtr,
                                          const ProgramValuePtr &LdVal,
                                          Instruction *Inst,
                                          const Condition &Cond) {
  propagateTaint(LdPtr, LdVal, Inst, Cond);

  const auto &Vals = getSymbolicVals(LdPtr);
  for (const auto &P : Vals) {
    const auto &Val = P.first;
    const auto &ValCond = P.second;
    if (IsaProperty<PropertySymExpr>(Val)) {
      const auto *SymVal = CastProperty<PropertySymExpr>(Val);
      bool ShouldPropagate =
          (SymVal->getUsedVars().size() > 1) || SymVal->isVar();
      if (ShouldPropagate) {
        for (const auto &SrcV : SymVal->getUsedVars()) {
          if (SrcV.getValue().getType()->isPointerTy()) {
            propagateTaint(SrcV.getValue(), LdVal, Inst, Cond && ValCond);
          }
        }
      }
    }
  }
}

Condition AnalysisState::getTaintedCond(const ProgramValuePtr &V) const {
  if (TaintedVals.hasValue(V)) {
    return TaintedVals.getGuardForValue(V);
  }
  return Condition::getFalseCond();
}

void AnalysisState::buildTaintSummary() {
  TaintValSet TaintedFormals;
  TaintValSet TaintedRets;

  for (auto Iter = Graph->arg_begin(), EIter = Graph->arg_end(); Iter != EIter;
       ++Iter) {
    ProgramValuePtr Arg(*Iter);
    TaintedFormals.addValue(Arg, getTaintedCond(Arg));
  }

  for (auto Iter = Graph->return_begin(), EIter = Graph->return_end();
       Iter != EIter; ++Iter) {
    ProgramValuePtr Ret(*Iter);
    TaintedRets.addValue(Ret, getTaintedCond(Ret));
  }

  TaintSmry = {TaintedFormals, TaintedRets, TaintedSteps};
}

void AnalysisState::taintProcessCall(CallInst *Inst, Function *Callee,
                                     const TaintSummary &Smry) {
  const auto &FormalToReal = FormalToRealMap.at(Inst);
  const auto &CalleeTaintedSteps = Smry.TaintedSteps;
  auto *GraphCS = Graph->findSite<GuardedValueFlowCallSite>(Inst);

  Condition BBCond = getLocalCond(Inst->getParent());
  for (const auto &P : Smry.TaintedFormals) {
    if (!FormalToReal.count(P.first)) {
      continue;
    }

    Condition Cond = transCond(Inst, Callee, P.second);
    Cond.andCond(BBCond);

    auto Steps = CalleeTaintedSteps.at(P.first);
    Steps.emplace_back(TaintStep::TAINT_STEP_CALL, Inst);
    taintVal(FormalToReal.at(P.first), Steps, Cond);
  }

  for (const auto &P : Smry.TaintedRets) {
    auto *RetNode = P.first.getAs<GuardedValueFlowNodeValue>()->getNode();
    const GuardedValueFlowNode *OutNode = nullptr;

    Condition Cond = transCond(Inst, Callee, P.second);
    Cond.andCond(BBCond);

    auto Steps = CalleeTaintedSteps.at(P.first);
    Steps.emplace_back(TaintStep::TAINT_STEP_CALL, Inst);

    if (RetNode->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
      OutNode = getNode(Inst);
    } else {
      OutNode = GraphCS->getPseudoOutput(
          Callee, cast<GuardedValueFlowReturnNode>(RetNode)->getIndex());
    }

    if (OutNode) {
      taintVal(OutNode, Steps, Cond);
    }
  }
}
