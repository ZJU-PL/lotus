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
        if (auto *ArgNode = getNode(Arg)) {
          taintVal(ArgNode, {}, Condition());
        }
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
    if (!SrcVal) {
      continue;
    }
    TaintStep SrcStep(TaintStep::TAINT_STEP_SOURCE, Inst, SrcVal);
    taintVal(V.getValue(), {SrcStep}, BBCond);
  }
}

void AnalysisState::taintVal(const ProgramValuePtr &V,
                             const std::vector<TaintStep> &Steps,
                             const Condition &PreCond, bool Peel) {
  if (V.isVacuous()) {
    return;
  }
  markTaint(V, Steps, PreCond);

  if (!Peel || !hasSymbolicVals(V)) {
    return;
  }

  Type *VTy = V.getType();
  if (!VTy) {
    return;
  }

  const auto &Vals = getSymbolicVals(V);
  for (const auto &P : Vals) {
    const auto &Val = P.first;
    const auto &Cond = P.second;
    if (IsaProperty<PropertySymExpr>(Val)) {
      const auto *SymVal = CastProperty<PropertySymExpr>(Val);

      bool ShouldPropagate = false;
      if (VTy->isIntegerTy()) {
        ShouldPropagate = (SymVal->getUsedVars().size() == 1);
      } else if (VTy->isPointerTy()) {
        ShouldPropagate = (SymVal->getUsedVars().size() > 1) || SymVal->isVar();
      }

      if (!ShouldPropagate) {
        continue;
      }

      for (const auto &DstV : SymVal->getUsedVars()) {
        Type *DstTy = DstV.getValue().getType();
        if (VTy->isIntegerTy() || (DstTy && DstTy->isPointerTy())) {
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
  if (DstV.isVacuous() || !DstV.getType()) {
    return;
  }
  bool IsDstPointer = DstV.getType()->isPointerTy();

  if (isInstUnmodelled(Inst)) {
    for (unsigned Idx = 0; Idx < Inst->getNumOperands(); ++Idx) {
      auto *OpNode = getNode(Inst->getOperand(Idx));
      if (!OpNode) {
        continue;
      }
      const auto &OpndVals = getSymbolicVals(OpNode);
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
      Type *SrcTy = SrcV.getType();
      bool ValuePassing = !IsDstPointer || (SrcTy && SrcTy->isPointerTy());
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
  if (Callee && Callee->isIntrinsic() && CS->arg_size() >= 2) {
    switch (Callee->getIntrinsicID()) {
    case Intrinsic::memset:
    case Intrinsic::memmove:
    case Intrinsic::memcpy: {
      auto *SrcNode = getNode(CS->getArgOperand(1));
      auto *DstNode = getNode(CS->getArgOperand(0));
      if (SrcNode && DstNode) {
        Res[SrcNode].insert(DstNode);
      }
      break;
    }
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
  auto F2RIt = FormalToRealMap.find(Inst);
  if (F2RIt == FormalToRealMap.end()) {
    return;
  }
  const auto &FormalToReal = F2RIt->second;
  const auto &CalleeTaintedSteps = Smry.TaintedSteps;
  auto *GraphCS = Graph->findSite<GuardedValueFlowCallSite>(Inst);

  Condition BBCond = getLocalCond(Inst->getParent());
  for (const auto &P : Smry.TaintedFormals) {
    auto RealIt = FormalToReal.find(P.first);
    if (RealIt == FormalToReal.end()) {
      continue;
    }

    Condition Cond = transCond(Inst, Callee, P.second);
    Cond.andCond(BBCond);

    std::vector<TaintStep> Steps;
    auto StepIt = CalleeTaintedSteps.find(P.first);
    if (StepIt != CalleeTaintedSteps.end()) {
      Steps = StepIt->second;
    }
    Steps.emplace_back(TaintStep::TAINT_STEP_CALL, Inst);
    taintVal(RealIt->second, Steps, Cond);
  }

  for (const auto &P : Smry.TaintedRets) {
    if (!P.first.isa<GuardedValueFlowNodeValue>()) {
      continue;
    }
    auto *RetNode = P.first.getAs<GuardedValueFlowNodeValue>()->getNode();
    if (!RetNode) {
      continue;
    }
    const GuardedValueFlowNode *OutNode = nullptr;

    Condition Cond = transCond(Inst, Callee, P.second);
    Cond.andCond(BBCond);

    std::vector<TaintStep> Steps;
    auto StepIt = CalleeTaintedSteps.find(P.first);
    if (StepIt != CalleeTaintedSteps.end()) {
      Steps = StepIt->second;
    }
    Steps.emplace_back(TaintStep::TAINT_STEP_CALL, Inst);

    if (RetNode->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
      if (!Inst->getType()->isVoidTy()) {
        OutNode = getNode(Inst);
      }
    } else if (GraphCS) {
      OutNode = GraphCS->getPseudoOutput(
          Callee, cast<GuardedValueFlowReturnNode>(RetNode)->getIndex());
    }

    if (OutNode) {
      taintVal(OutNode, Steps, Cond);
    }
  }
}
