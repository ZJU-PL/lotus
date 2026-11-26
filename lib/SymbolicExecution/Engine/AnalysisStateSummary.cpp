//===----------------------------------------------------------------------===//
//
// AnalysisState interprocedural summary import/export implementation.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Engine/AnalysisState.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"
#include "SymbolicExecution/Engine/AnalysisSummary.h"

#include <cassert>

using namespace llvm;
using namespace SymbolicExecution;

void AnalysisState::finalizeSummary() {
  processReturn();
  buildTaintSummary();
  if (Ext) {
    FinalizedExt = Ext->finalizeSummary(*this);
  }
}

void AnalysisState::processCall(CallInst *Inst, Function *Callee,
                                const AnalysisSummary &Smry) {
  initializeFormalToRealMap(Inst, Smry);

  auto *GraphCS = Graph->findSite<GuardedValueFlowCallSite>(Inst);
  auto *CalleeGraph = Smry.getGraph();

  addFormalToRealArgMap(GraphCS, Callee, Smry);
  addFormalToRealLenMap(GraphCS, Callee, Smry);
  addFormalToRealEscapeMap(GraphCS, Callee, Smry);

  for (auto Iter = GraphCS->input_begin(Callee),
            EIter = GraphCS->input_end(Callee);
       Iter != EIter; ++Iter) {
    auto *InputNode = (*Iter).InputNode;
    if ((*Iter).IsCommonInput) {
      initSymbol(InputNode);
      continue;
    }

    assert(isa<GuardedValueFlowCallOutputNode>(InputNode));
    assert(InputNode->getNumChildren() == 1);

    auto *LdMemNode = cast<GuardedValueFlowNode>(InputNode->getChild(0));
    ProgramValuePtr BasePtr;
    auto *LdPtr = cast<GuardedValueFlowCallOutputNode>(InputNode)
                      ->getAccessPath()
                      .get_base_ptr();

    auto *LdPtrNode = CalleeGraph->findNode(LdPtr);
    if (LdPtrNode && hasFormal(Inst, LdPtrNode)) {
      BasePtr = getRealForFormal(Inst, LdPtrNode);
    }

    processLoadPtr(BasePtr, LdMemNode, InputNode, Inst);
    initSymbol(InputNode);
  }

  for (const auto &P : FormalToRealArgMap.at(Inst)) {
    addFormalToRealValues(GraphCS, Callee, P.first, P.second);
  }

  Condition CSCond = getLocalCond(Inst->getParent());
  const auto &EscapeInfo = Smry.getEscapeInfo();
  for (const auto &P : FormalToRealEscapeMap.at(Inst)) {
    const auto &Formal = P.first;
    const auto &Real = P.second;

    initSymbol(Real);
    addFormalToRealValues(GraphCS, Callee, Formal, Real);

    assert(EscapeInfo.count(Formal));
    auto AccSizes = inlineVals(EscapeInfo.at(Formal), Inst, Callee, CSCond);
    AccSizes.forEach([&](const PropertyValuePtr &Sz, const Condition &C) {
      createMemoryObject(Real, PTItem::MK_CONCRETE, Sz, C);
    });
  }

  const auto &SmryOutputPts = Smry.OutputPts;
  const auto &RetSymbolicValMap = Smry.getOutSymbolicValMap();

  for (auto Iter = CalleeGraph->return_begin(),
            EIter = CalleeGraph->return_end();
       Iter != EIter; ++Iter) {
    auto *RetNode = *Iter;
    const GuardedValueFlowNode *OutNode = nullptr;
    if (RetNode->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
      OutNode = getNode(Inst);
    } else {
      OutNode = GraphCS->getPseudoOutput(
          Callee, cast<GuardedValueFlowReturnNode>(RetNode)->getIndex());
    }

    if (SmryOutputPts.count(RetNode)) {
      const auto &RetPts = SmryOutputPts.at(RetNode);
      auto InlinedPts = inlineVals(RetPts, Inst, Callee, CSCond).first;
      setPts(OutNode, InlinedPts);
    }
  }

  StrState.onProcessCall(Inst, Callee, *this, Smry.StrState);
  for (const auto &P : FormalToRealLenMap.at(Inst)) {
    addFormalToRealValues(GraphCS, Callee, P.first, P.second);
  }

  for (auto Iter = CalleeGraph->return_begin(),
            EIter = CalleeGraph->return_end();
       Iter != EIter; ++Iter) {
    auto *RetNode = *Iter;
    const GuardedValueFlowNode *OutNode = nullptr;
    if (RetNode->getKind() == GuardedValueFlowNode::Kind::CommonReturn) {
      OutNode = getNode(Inst);
    } else {
      OutNode = GraphCS->getPseudoOutput(
          Callee, cast<GuardedValueFlowReturnNode>(RetNode)->getIndex());
    }

    const auto &RetVals = RetSymbolicValMap.at(RetNode);
    GuardedSymbolicValSet InlinedVals =
        inlineVals(RetVals, Inst, Callee, CSCond);

    if (InlinedVals.empty()) {
      auto ReceiverVal = GetProperty<PropertySymExpr>(Var(OutNode));
      Regs[OutNode].addValue(ReceiverVal);
    } else {
      Regs[OutNode].setValues(InlinedVals);
    }
  }

  taintProcessCall(Inst, Callee, Smry.getTaintSmry());
  if (Ext) {
    Ext->onCallSummary(Inst, Callee, Smry.getExtension(), *this);
  }
}

void AnalysisState::addFormalToRealLenMap(GuardedValueFlowCallSite *GraphCS,
                                          Function *Callee,
                                          const AnalysisSummary &Smry) {
  (void)Callee;
  auto *CS = GraphCS->getInstruction();
  for (const auto &P : Smry.getStrState().getLenPts()) {
    const auto &LenV = P.first;
    ProgramValuePtr LenVAtCaller = CStringState::getLenVAtCaller(LenV, CS);
    FormalToRealLenMap[CS].insert(std::make_pair(LenV, LenVAtCaller));
    FormalToRealMap[CS].insert(std::make_pair(LenV, LenVAtCaller));
  }
}

void AnalysisState::addFormalToRealEscapeMap(GuardedValueFlowCallSite *GraphCS,
                                             Function *Callee,
                                             const AnalysisSummary &Smry) {
  (void)Callee;
  (void)Smry;
  FormalToRealEscapeMap[GraphCS->getInstruction()];
}

bool AnalysisState::hasFormal(Instruction *CS,
                              const ProgramValuePtr &Formal) const {
  return FormalToRealMap.count(CS) && FormalToRealMap.at(CS).count(Formal);
}

ProgramValuePtr
AnalysisState::getRealForFormal(Instruction *CS,
                                const ProgramValuePtr &Formal) const {
  return FormalToRealMap.at(CS).at(Formal);
}

void AnalysisState::addFormalToRealArgMap(GuardedValueFlowCallSite *GraphCS,
                                          Function *Callee,
                                          const AnalysisSummary &Smry) {
  auto *CS = GraphCS->getInstruction();
  auto *CalleeGraph = Smry.getGraph();
  for (auto Iter = GraphCS->input_begin(Callee),
            EIter = GraphCS->input_end(Callee);
       Iter != EIter; ++Iter) {
    auto *InputNode = (*Iter).InputNode;
    bool IsCommon = (*Iter).IsCommonInput;
    if (IsCommon) {
      auto Index = (*Iter).InputIndex;
      if (Index >= CalleeGraph->getNumCommonArgument()) {
        for (size_t I = 0; I < CalleeGraph->getNumVarArgument(); ++I) {
          ProgramValuePtr Formal(CalleeGraph->getVarArgument(I));
          FormalToRealArgMap[CS].insert(std::make_pair(Formal, InputNode));
          FormalToRealMap[CS].insert(std::make_pair(Formal, InputNode));
        }
      } else {
        ProgramValuePtr Formal(CalleeGraph->getCommonArgument(Index));
        FormalToRealArgMap[CS].insert(std::make_pair(Formal, InputNode));
        FormalToRealMap[CS].insert(std::make_pair(Formal, InputNode));
      }
    } else {
      auto Index = (*Iter).InputIndex;
      assert(Index < CalleeGraph->getNumPseudoArgument());
      ProgramValuePtr Formal(CalleeGraph->getPseudoArgument(Index));
      FormalToRealArgMap[CS].insert(std::make_pair(Formal, InputNode));
      FormalToRealMap[CS].insert(std::make_pair(Formal, InputNode));
    }
  }
}

void AnalysisState::addFormalToRealValues(GuardedValueFlowCallSite *GraphCS,
                                          Function *Callee,
                                          const ProgramValuePtr &Formal,
                                          const ProgramValuePtr &Real) {
  auto *CS = GraphCS->getInstruction();
  auto &FormalToRealSymM = FormalToRealSymValsMap[CS];
  auto &SMTMap = SMTRenameCtxMap[Callee];

  GuardedSymbolicValSet RealSyms;
  if (Real.isConstant()) {
    RealSyms.addValue(GetProperty<PropertyInteger>(Real.getAsConstant()));
  } else {
    assert(Regs.count(Real));
    RealSyms.addValues(Regs.at(Real));
  }
  FormalToRealSymM[Formal].addValues(RealSyms);

  if (!SMTMap.count(Formal)) {
    auto LE = Solver->getExpr(Var(Formal));
    assert(LE.isBitVector());
    assert(LE.getBitVecSize());
    SMTMap.insert(std::make_pair(Formal, RenameCtx(LE)));
  }

  auto &Rename = SMTMap.at(Formal);
  unsigned LSz = Rename.getFormalExpr().getBitVecSize();
  SMTExpr RE = Solver->getExpr(Var(Real));
  assert(RE.isBitVector());

  unsigned RSz = RE.getBitVecSize();
  if (LSz == RSz) {
    Rename.add(CS, RE);
  } else if (LSz > RSz) {
    auto Temp = RE.basic_sext(LSz - RSz);
    assert(Temp.getBitVecSize() == LSz);
    Rename.add(CS, Temp);
  } else {
    auto Temp = RE.basic_extract(LSz - 1, 0);
    assert(Temp.getBitVecSize() == LSz);
    Rename.add(CS, Temp);
  }
}

void AnalysisState::initializeFormalToRealMap(CallInst *Inst,
                                              const AnalysisSummary &Smry) {
  FormalToRealMap[Inst];
  FormalToRealSymValsMap[Inst];
  FormalToRealArgMap[Inst];
  FormalToRealLenMap[Inst];
  FormalToRealEscapeMap[Inst];
  SMTRenameCtxMap[Smry.getFunc()];
}

Condition AnalysisState::transCond(Instruction *CS, Function *Callee,
                                   const Condition &CalleeCond) const {
  if (CalleeCond.isLit()) {
    return CalleeCond;
  }

  int64_t CondIndex = CalleeCond.getIndex();
  if (InlineCondCache.count(CS) && InlineCondCache.at(CS).count(CondIndex)) {
    return InlineCondCache.at(CS).at(CondIndex);
  }

  bool ToUnknown = false;
  Condition CurCons(CalleeCond.translate(getSolver(), ToUnknown));
  if (!ToUnknown) {
    std::string CSSuffix = getCallsiteSuffix(CS);
    CurCons = CurCons.rename(CSSuffix);
    CurCons = CurCons && getMappingCond(CS, Callee);
  }

  InlineCondCache[CS][CondIndex] = CurCons;
  return CurCons;
}

GuardedSymbolicValSet
AnalysisState::inlineVals(const GuardedSymbolicValSet &Vals, Instruction *CS,
                          Function *Callee, const Condition &CSCond) const {
  return Vals.reduce<GuardedSymbolicValSet>(
      [&](const PropertyValuePtr &Val, const Condition &C,
          GuardedSymbolicValSet &Res) {
        auto InlinedV = inlineExpr(Val, CS);
        Res.addValues(InlinedV && transCond(CS, Callee, C) && CSCond);
      },
      [](const GuardedSymbolicValSet &Res) { return Res.isFull(); });
}

std::pair<PtsSet, bool>
AnalysisState::inlineVals(const PtsSet &Vals, Instruction *CS, Function *Callee,
                          const Condition &CSCond) const {
  const auto &FormalToReal = FormalToRealMap.at(CS);
  bool Degenerate = false;

  auto PtsRes = Vals.reduce<PtsSet>(
      [&](const PTItem &Pt, const Condition &CalleeCond, PtsSet &Res) {
        if (Res.isFull()) {
          return;
        }

        auto Base = Pt.getAllocSite();
        auto BaseOff = Pt.getOffset();
        if (FormalToReal.count(Base)) {
          const auto &CallerBase = FormalToReal.at(Base);
          if (!hasPts(CallerBase)) {
            if (mustBeConstantInt(CallerBase)) {
              Degenerate = true;
            }
            return;
          }

          auto PreCond = transCond(CS, Callee, CalleeCond) && CSCond;
          auto CallerOffsets = inlineExpr(BaseOff, CS);
          auto CallerPts =
              getPts(CallerBase).offsetBy(CallerOffsets) && PreCond;
          Res.addValues(CallerPts);
        }
      },
      [](const PtsSet &Res) { return Res.isFull(); });

  Degenerate = Degenerate && PtsRes.empty();
  return std::make_pair(PtsRes, Degenerate);
}

GuardedProgramValSet AnalysisState::inlineVals(const GuardedProgramValSet &Vals,
                                               Instruction *CS,
                                               Function *Callee) const {
  const auto &FormalToReal = FormalToRealMap.at(CS);
  return Vals.reduce<GuardedProgramValSet>(
      [&](const ProgramValuePtr &V, const Condition &CalleeCond,
          GuardedProgramValSet &Res) {
        if (FormalToReal.count(V)) {
          Res.addValue(FormalToReal.at(V), transCond(CS, Callee, CalleeCond));
        }
      },
      [](const GuardedProgramValSet &Res) { return Res.isFull(); });
}
