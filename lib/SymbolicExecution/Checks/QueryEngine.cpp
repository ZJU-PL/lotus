//===----------------------------------------------------------------------===//
//
// QueryEngine: dispatch, unified interprocedural query inlining, and MPA batch
// discharging.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Checks/QueryEngine.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"
#include "SymbolicExecution/Models/MemoryAPI.h"
#include "SymbolicExecution/Support/AnalysisContext.h"
#include "SymbolicExecution/Support/AnalysisLimit.h"

#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <regex>
#include <sstream>

#define DEBUG_TYPE "Symex"

using namespace llvm;
using namespace SymbolicExecution;

namespace {

std::string normalizeConditionForGrouping(const Condition &Cond) {
  if (Cond.isLit()) {
    return Cond.isTrue() ? "true" : "false";
  }

  std::ostringstream OSS;
  OSS << Cond.getSMTConstr();
  static const std::regex SuffixPattern(R"(_0x[0-9a-fA-F]+)");
  return std::regex_replace(OSS.str(), SuffixPattern, "");
}

} // namespace

void QuerySummary::translate(PathCondSolver *NewSolver) {
  for (auto &P : QueryToTraces) {
    QuerySet &V = P.first;
    for (auto &Entry : V) {
      Entry.first.translate(NewSolver);
    }
    V.translate(NewSolver);
  }
}

QueryEngine::QueryEngine(unsigned EnabledBugs, BugReportCollector &Collector)
    : EnabledBugs(EnabledBugs), Collector(Collector) {}

void QueryEngine::onPreInstruction(Instruction *Inst,
                                   const AnalysisState &State) {
  if ((EnabledBugs & BUG_TY_DOUBLE_FREE) && isa<CallInst>(Inst)) {
    Function *Callee = getCallee(Inst);
    if (isFreeLikeFunction(Callee)) {
      buildDoubleFreeQuery(Inst, State);
    }
  }
}

void QueryEngine::onPostInstruction(Instruction *Inst,
                                    const AnalysisState &State) {
  auto OpC = Inst->getOpcode();
  if (OpC == Instruction::Load && (EnabledBugs & BUG_TY_BOF)) {
    auto *LoadI = cast<LoadInst>(Inst);
    buildBofQueryLoadStore(Inst, State.getNode(getPointerOperand(LoadI)),
                           LoadI->getType(), State);
  } else if (OpC == Instruction::Store && (EnabledBugs & BUG_TY_BOF)) {
    auto *StoreI = cast<StoreInst>(Inst);
    buildBofQueryLoadStore(Inst, State.getNode(getPointerOperand(StoreI)),
                           StoreI->getValueOperand()->getType(), State);
  } else if (State.isInstUnmodelled(Inst)) {
    auto *Dst = State.getNode(Inst);
    for (unsigned Idx = 0; Idx < Inst->getNumOperands(); ++Idx) {
      auto OpndVals = State.getSymbolicVals(State.getNode(Inst->getOperand(Idx)));
      for (const auto &P : OpndVals) {
        if (IsaProperty<PropertySymExpr>(P.first)) {
          for (const auto &DepV :
               CastProperty<PropertySymExpr>(P.first)->getUsedVars()) {
            addExtraDeps(Dst, DepV.getValue(), P.second);
          }
        }
      }
    }
  } else if (OpC == Instruction::Call) {
    if (!isDefiniteCall(Inst) && (EnabledBugs & BUG_TY_BOF)) {
      buildBofQueryLibCall(cast<CallInst>(Inst), State);
    }
  }

  if ((EnabledBugs & BUG_TY_DBZ) &&
      (OpC == Instruction::SDiv || OpC == Instruction::UDiv ||
       OpC == Instruction::SRem || OpC == Instruction::URem)) {
    buildDbzQuery(Inst, State);
  }

  if (OpC == Instruction::Add || OpC == Instruction::Sub ||
      OpC == Instruction::Mul || OpC == Instruction::Shl) {
    if (EnabledBugs & BUG_TY_INT_OVERFLOW) {
      buildIntBoundQuery(Inst, BUG_TY_INT_OVERFLOW, true, State);
    }
    if (EnabledBugs & BUG_TY_INT_UNDERFLOW) {
      buildIntBoundQuery(Inst, BUG_TY_INT_UNDERFLOW, false, State);
    }
    if (EnabledBugs & BUG_TY_SIGNED_INT_OVERFLOW) {
      buildIntBoundQuery(Inst, BUG_TY_SIGNED_INT_OVERFLOW, true, State);
    }
    if (EnabledBugs & BUG_TY_SIGNED_INT_UNDERFLOW) {
      buildIntBoundQuery(Inst, BUG_TY_SIGNED_INT_UNDERFLOW, false, State);
    }
  }

  if ((EnabledBugs & BUG_TY_SHIFT_OVERFLOW) &&
      (OpC == Instruction::Shl || OpC == Instruction::LShr ||
       OpC == Instruction::AShr)) {
    buildShiftOverflowQuery(Inst, State);
  }

  if ((EnabledBugs & BUG_TY_NULL_DEREF) &&
      (OpC == Instruction::Load || OpC == Instruction::Store)) {
    buildNullDerefQuery(Inst, State);
  }

  if ((EnabledBugs & BUG_TY_ARRAY_INDEX_OOB) &&
      OpC == Instruction::GetElementPtr) {
    buildArrayIndexOOBQuery(Inst, State);
  }

  if ((EnabledBugs & BUG_TY_UNINIT_READ) && OpC == Instruction::Load) {
    buildUninitializedReadQuery(Inst, State);
  }

  if ((EnabledBugs & BUG_TY_UAF) &&
      (OpC == Instruction::Load || OpC == Instruction::Store)) {
    buildUafQuery(Inst, State);
  }

  if ((EnabledBugs & BUG_TY_NEGATIVE_ARRAY_INDEX) &&
      OpC == Instruction::GetElementPtr) {
    buildNegativeArrayIndexQuery(Inst, State);
  }

  if ((EnabledBugs & BUG_TY_INT_TRUNCATION) &&
      (OpC == Instruction::Trunc || OpC == Instruction::ZExt ||
       OpC == Instruction::SExt || OpC == Instruction::FPToSI ||
       OpC == Instruction::FPToUI)) {
    buildIntTruncationQuery(Inst, State);
  }
}

void QueryEngine::onCallSummary(CallInst *Inst, Function *Callee,
                                const SummaryExtension *CalleeSummary,
                                const AnalysisState &State) {
  auto *QSmry = dynamic_cast<const QuerySummary *>(CalleeSummary);
  if (!QSmry) {
    return;
  }

  for (const auto &P : QSmry->getQueries()) {
    auto Trace = P.second;
    Trace.emplace_back(TraceStep::TRACE_STEP_CALL, Inst, Inst);

    auto InlinedQs = inlineQuerySet(P.first, Inst, Callee, State);
    InlinedQs.forEach([&](const NumericalQueryPtr &Q, const Condition &Cond) {
      addQueryTrace(Q, Cond, Trace, State);
    });
  }
}

std::unique_ptr<SummaryExtension>
QueryEngine::finalizeSummary(const AnalysisState &State) {
  dischargeQueries(State);
  return std::make_unique<QuerySummary>(std::move(QueryToTraces));
}

void QueryEngine::addExtraDeps(const ProgramValuePtr &Dst,
                               const ProgramValuePtr &Src,
                               const Condition &Cond) {
  if (Src.isConstant()) {
    return;
  }
  ExtraDepsMap[Dst].addValue(Src, Cond);
}

GuardedProgramValSet QueryEngine::getDepsVals(const PropertyValuePtr &V) const {
  if (IsaProperty<PropertyInteger>(V)) {
    return {};
  }

  GuardedProgramValSet Res;
  const auto *SymExpr = CastProperty<PropertySymExpr>(V);
  for (const auto &UsedV : SymExpr->getUsedVars()) {
    auto It = ExtraDepsMap.find(UsedV.getValue());
    if (It != ExtraDepsMap.end()) {
      Res.addValues(It->second);
    }
  }
  return Res;
}

void QueryEngine::addQueryTrace(const NumericalQueryPtr &Q,
                                const Condition &Cond, const TraceStep &Step,
                                const AnalysisState &State) {
  addQueryTrace(Q, Cond, std::vector<TraceStep>{Step}, State);
}

void QueryEngine::addQueryTrace(const NumericalQueryPtr &Q,
                                const Condition &Cond,
                                const std::vector<TraceStep> &Trace,
                                const AnalysisState &State) {
  if (QueryCount >= AnalysisLimit::FUNC_QUERY_LIMIT_V) {
    return;
  }

  auto *CurSite = Trace.back().Inst;
  const auto &SiteBBCond = State.getLocalCond(CurSite->getParent());

  QuerySet Qs;
  if (Qs.addValue(Q, Cond && SiteBBCond)) {
    ++QueryCount;

    auto *SinkInst = Trace.front().Inst;
    auto *SinkFunc = SinkInst->getParent()->getParent();
    if (AnalysisLimit::getDebugFunctionName() == SinkFunc->getName()) {
      LLVM_DEBUG(dbgs() << "[DEBUG-Numerical] query generated at "
                        << SinkFunc->getName();
                 if (Trace.size() > 1) {
                   dbgs() << " inlined at "
                          << CurSite->getParent()->getParent()->getName();
                 } dbgs() << "\n";
                 SinkInst->print(dbgs(), true); dbgs() << "\n"; Q->dump(););
    }

    QueryToTraces.emplace_back(std::move(Qs), Trace);
  }
}

QuerySet QueryEngine::inlineQuerySet(const QuerySet &Vals, Instruction *CS,
                                     Function *Callee,
                                     const AnalysisState &State) const {
  return Vals.reduce<QuerySet>(
      [&](const NumericalQueryPtr &Q, const Condition &CalleeCond,
          QuerySet &Res) {
        auto *QPtr = Q.get();

        QuerySet InlinedQs;
        if (InlineQueryCache.count(QPtr) &&
            InlineQueryCache.at(QPtr).count(CS)) {
          InlinedQs = InlineQueryCache.at(QPtr).at(CS);
        } else {
          InlinedQs = inlineQuery(Q, CS, Callee, State);
          InlineQueryCache[QPtr][CS] = InlinedQs;
        }

        InlinedQs = InlinedQs && State.transCond(CS, Callee, CalleeCond);
        Res.addValues(InlinedQs);
      },
      [](const QuerySet &Res) { return Res.isFull(); });
}

QuerySet QueryEngine::inlineQuery(const NumericalQueryPtr &Q, Instruction *CS,
                                  Function *Callee,
                                  const AnalysisState &State) const {
  const NumericalQuery *QPtr = Q.get();
  if (const auto *DQ = dyn_cast<DirectNumericalQuery>(QPtr)) {
    return inlineDirectQuery(DQ, CS, Callee, State);
  }
  return inlineIndirectBofQuery(cast<IndirectNumericalQuery>(QPtr), CS, Callee,
                                State);
}

QuerySet QueryEngine::inlineDirectQuery(const DirectNumericalQuery *DQ,
                                        Instruction *CS, Function *Callee,
                                        const AnalysisState &State) const {
  if (DQ->mustSat()) {
    return DQ->clone();
  }

  GuardedProgramValSet InlinedDeps = State.inlineVals(DQ->getDeps(), CS, Callee);
  if (!DQ->getSymExpr()) {
    QuerySet Res;
    Res.addValue(
        DirectNumericalQuery::createTaintOnly(DQ->getBugTy(), InlinedDeps));
    return Res;
  }

  QuerySet Res;
  const auto &InlinedExprs = State.inlineExpr(DQ->getSymExpr(), CS);
  for (const auto &P : InlinedExprs) {
    const auto &Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(DirectNumericalQuery::createSymbolic(
                       DQ->getBugTy(), Expr, DQ->isEquation(), InlinedDeps),
                   P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      bool ResolvedToErr =
          DQ->isEquation() ? (Expr == int64_t(0)) : (Expr > int64_t(0));
      if (ResolvedToErr) {
        Res.addValue(DirectNumericalQuery::createMustErr(DQ->getBugTy()),
                     P.second);
      }
    }
  }
  return Res;
}

QuerySet QueryEngine::inlineIndirectBofQuery(const IndirectNumericalQuery *IDQ,
                                             Instruction *CS, Function *Callee,
                                             const AnalysisState &State) const {
  GuardedProgramValSet InlinedDeps =
      State.inlineVals(IDQ->getDeps(), CS, Callee);

  auto BasePtr = IDQ->getBasePtr();
  auto Offset = IDQ->getOffset();
  auto AccSize = IDQ->getAccSize();

  PtsSet BaseAtCaller;
  if (State.hasFormal(CS, BasePtr)) {
    auto Real = State.getRealForFormal(CS, BasePtr);
    if (State.hasPts(Real)) {
      BaseAtCaller.addValues(State.getPts(Real));
    } else if (State.mustBeConstantInt(Real)) {
      return DirectNumericalQuery::createMustErr(BUG_TY_BOF);
    }
  }

  if (BaseAtCaller.empty()) {
    return {};
  }

  auto InlinedOffsets = State.inlineExpr(Offset, CS);
  GuardedSymbolicValSet InlinedAccSizes;
  if (AccSize) {
    InlinedAccSizes = State.inlineExpr(AccSize, CS);
  }

  QuerySet Res;
  for (const auto &P : BaseAtCaller) {
    const auto &Pt = P.first;
    const auto &PtCond = P.second;
    if (!needsMemoryCheck(Pt)) {
      continue;
    }

    auto CallerOffsets = InlinedOffsets + Pt.getOffset();
    if (Pt.isSymbolic()) {
      for (const auto &OffCond : CallerOffsets) {
        const auto &Off = OffCond.first;
        if (!AccSize) {
          Res.addValue(IndirectNumericalQuery::createBof(
                           Pt.getAllocSite(), Off, PropertyValuePtr(),
                           getDepsVals(Off).merge(InlinedDeps)),
                       PtCond && OffCond.second);
          if (Res.isFull()) {
            return Res;
          }
          continue;
        }

        for (const auto &AccSzCond : InlinedAccSizes) {
          const auto &AccSz = AccSzCond.first;
          Res.addValue(IndirectNumericalQuery::createBof(
                           Pt.getAllocSite(), Off, AccSz,
                           getDepsVals(Off).merge(
                               getDepsVals(AccSz).merge(InlinedDeps))),
                       PtCond && OffCond.second && AccSzCond.second);
          if (Res.isFull()) {
            return Res;
          }
        }
      }
    } else {
      assert(Pt.isConcrete());
      for (const auto &OffCond : CallerOffsets) {
        const auto &CallerOff = OffCond.first;
        if (CallerOff < int64_t(0)) {
          Res.addValue(DirectNumericalQuery::createMustErr(BUG_TY_BOF),
                       PtCond && OffCond.second);
          continue;
        }

        if (!AccSize) {
          Res.addValue(
              DirectNumericalQuery::createTaintOnly(BUG_TY_BOF, InlinedDeps),
              PtCond && OffCond.second);
          continue;
        }

        for (const auto &AccSzCond : InlinedAccSizes) {
          auto Diff = CallerOff + AccSzCond.first - Pt.getSize();
          if (IsaProperty<PropertyInteger>(Diff)) {
            if (Diff > int64_t(0)) {
              Res.addValue(DirectNumericalQuery::createMustErr(BUG_TY_BOF),
                           PtCond && OffCond.second && AccSzCond.second);
            }
          } else {
            Res.addValue(DirectNumericalQuery::createSymbolic(
                             BUG_TY_BOF, Diff, false,
                             getDepsVals(Diff).merge(InlinedDeps)),
                         PtCond && OffCond.second && AccSzCond.second);
          }

          if (Res.isFull()) {
            return Res;
          }
        }
      }
    }
  }

  return Res;
}

bool QueryEngine::tryReportQuery(const NumericalQueryPtr &QPtr,
                                 const Condition &QDepCond, Function *SinkFun,
                                 const std::vector<TraceStep> &Trace,
                                 const AnalysisState &State) {
  auto *Solver = State.getSolver();
  auto *Q = QPtr.get();
  const auto *DQ = dyn_cast<DirectNumericalQuery>(Q);
  bool Reported = false;
  bool Filtered = false;

  if (DQ && DQ->mustSat()) {
    SMTExprVec DepConstr = QDepCond.toSMT(*Solver);
    if (Solver->isConstraintSat(DepConstr)) {
      if (AnalysisLimit::getDebugFunctionName() == SinkFun->getName()) {
        LLVM_DEBUG(dbgs() << "Query must err, adding bug report in func "
                          << State.getFunc()->getName() << "\n");
      }
      Collector.addReport({Q->getBugTy(), {}, Trace});
      Reported = true;
    } else {
      if (AnalysisLimit::getDebugFunctionName() == SinkFun->getName()) {
        LLVM_DEBUG(dbgs() << "Query must err, filtered bug report in func "
                          << State.getFunc()->getName() << "\n");
      }
      Filtered = true;
    }
    return Reported || Filtered;
  }

  const bool CanCheckNow =
      (DQ != nullptr) ||
      (isa<IndirectNumericalQuery>(Q) &&
       State.getContext().isFunctionTopLevel(State.getFunc()));
  if (!CanCheckNow) {
    return false;
  }

  const auto &TaintedVals = State.getTaintedVals();
  const auto &TaintedSteps = State.getTaintedSteps();
  std::vector<TaintStep> TaintSteps;
  bool QueryConsBuilt = false;
  SMTExprVec QueryCons = Solver->createEmptySMTExprVec();

  for (const auto &VCond : Q->getUsedVals()) {
    const auto &V = VCond.first;
    if (!TaintedVals.hasValue(V)) {
      continue;
    }

    auto StepIt = TaintedSteps.find(V);
    if (StepIt != TaintedSteps.end()) {
      TaintSteps = StepIt->second;
    }

    auto TaintCond = TaintedVals.getGuardForValue(V) && VCond.second;
    SMTExprVec TaintCons = TaintCond.toSMT(*Solver);

    if (!QueryConsBuilt) {
      SMTExprVec DepConstr = QDepCond.toSMT(*Solver);
      if (DQ) {
        QueryCons = DQ->toSMT(*Solver);
        QueryCons.mergeWithAnd(DepConstr);
      } else {
        QueryCons = DepConstr;
      }
      QueryConsBuilt = true;
    }

    TaintCons.mergeWithAnd(QueryCons);

    if (Solver->isConstraintSat(TaintCons)) {
      Collector.addReport({Q->getBugTy(), TaintSteps, Trace});
      Reported = true;
      break;
    }
    Filtered = true;
  }

  return Reported || Filtered;
}

void QueryEngine::dischargeQueries(const AnalysisState &State) {
  auto *Solver = State.getSolver();
  QueryTraceList RemainingQueries;

  struct QueryInfo {
    NumericalQueryPtr Query;
    Condition QDepCond;
    std::vector<TraceStep> Trace;
    Function *SinkFun;
    SymexBugType BugTy;
  };

  std::vector<QueryInfo> AllQueries;
  std::map<std::string, std::vector<size_t>> ContextGroups;

  for (const auto &P : QueryToTraces) {
    const auto &Trace = P.second;
    auto *SinkFun = Trace.front().Inst->getParent()->getParent();

    assert(P.first.size() == 1);
    for (const auto &QCond : P.first) {
      QueryInfo Info{QCond.first, QCond.second, Trace, SinkFun,
                     QCond.first->getBugTy()};
      size_t Idx = AllQueries.size();
      AllQueries.push_back(std::move(Info));

      std::string NormalizedKey = normalizeConditionForGrouping(QCond.second);
      ContextGroups[NormalizedKey].push_back(Idx);
    }
  }

  std::vector<bool> Resolved(AllQueries.size(), false);

  for (const auto &Group : ContextGroups) {
    const auto &Indices = Group.second;
    if (Indices.size() == 1) {
      size_t Idx = Indices[0];
      const auto &Info = AllQueries[Idx];
      Resolved[Idx] = tryReportQuery(Info.Query, Info.QDepCond, Info.SinkFun,
                                     Info.Trace, State);
      continue;
    }

    const Condition &SharedContext = AllQueries[Indices[0]].QDepCond;
    SMTExprVec Context = SharedContext.toSMT(*Solver);

    std::vector<size_t> MustSatIndices;
    std::vector<size_t> RegularIndices;
    for (size_t Idx : Indices) {
      const auto &Info = AllQueries[Idx];
      auto *DQ = dyn_cast<DirectNumericalQuery>(Info.Query.get());
      if (DQ && DQ->mustSat()) {
        MustSatIndices.push_back(Idx);
      } else {
        RegularIndices.push_back(Idx);
      }
    }

    if (!MustSatIndices.empty()) {
      bool ContextSat = Solver->isConstraintSat(Context);
      for (size_t Idx : MustSatIndices) {
        const auto &Info = AllQueries[Idx];
        if (ContextSat) {
          Collector.addReport({Info.BugTy, {}, Info.Trace});
        }
        Resolved[Idx] = true;
      }
    }

    if (!RegularIndices.empty()) {
      std::vector<SMTExprVec> Predicates;
      Predicates.reserve(RegularIndices.size());

      for (size_t Idx : RegularIndices) {
        const auto &Info = AllQueries[Idx];
        auto *DQ = dyn_cast<DirectNumericalQuery>(Info.Query.get());
        if (DQ && !DQ->mustSat()) {
          Predicates.push_back(DQ->toSMT(*Solver));
        } else {
          Predicates.push_back(Solver->createEmptySMTExprVec());
        }
      }

      std::vector<bool> BatchResults =
          Solver->batchCheckPredicates(Context, Predicates);

      for (size_t I = 0; I < RegularIndices.size(); ++I) {
        size_t Idx = RegularIndices[I];
        if (!BatchResults[I]) {
          Resolved[Idx] = true;
          continue;
        }
        const auto &Info = AllQueries[Idx];
        Resolved[Idx] = tryReportQuery(Info.Query, Info.QDepCond, Info.SinkFun,
                                       Info.Trace, State);
      }
    }
  }

  for (size_t I = 0; I < AllQueries.size(); ++I) {
    if (!Resolved[I]) {
      const auto &Info = AllQueries[I];
      QuerySet Qs;
      Qs.addValue(Info.Query, Info.QDepCond);
      RemainingQueries.emplace_back(std::move(Qs), Info.Trace);
    }
  }

  QueryToTraces = std::move(RemainingQueries);
}
