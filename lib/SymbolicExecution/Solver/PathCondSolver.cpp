#include "SymbolicExecution/Solver/PathCondSolver.h"

#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"

#include "llvm/ADT/StringExtras.h"

#include <vector>

using namespace llvm;
using namespace SymbolicExecution;

namespace {

bool shouldUseSignExtension(unsigned Pred) {
  switch (Pred) {
  case CmpInst::ICMP_UGE:
  case CmpInst::ICMP_UGT:
  case CmpInst::ICMP_ULE:
  case CmpInst::ICMP_ULT:
    return false;
  default:
    return true;
  }
}

void extendExprWidth(SMTExpr &Expr, unsigned ExtWidth, bool SignExtend) {
  Expr =
      SignExtend ? Expr.array_sext(ExtWidth, 1) : Expr.array_zext(ExtWidth, 1);
}

} // namespace

std::atomic<uint32_t> PathCondSolver::ID{0};

PathCondSolver::PathCondSolver(const llvm::DataLayout &DL)
    : DL(DL), SolverID(ID++),
      GraphSolver(std::make_unique<GuardedValueFlowSolver>(Fctry, DL)) {}

SMTExpr PathCondSolver::buildRegionCondition(GuardedValueFlowRegionNode *Cond) {
  resetSolverState();

  auto Res = GraphSolver->getDataDeps(Cond);
  Res.push_back(GraphSolver->getOrInsertExpr(Cond).bv12bool());
  updateUsedCSOuts();

  return Res.toAndExpr();
}

SMTExprVec PathCondSolver::createEmptySMTExprVec() {
  return Fctry.createEmptySMTExprVec();
}

SMTExpr PathCondSolver::getExpr(Var V) {
  resetSolverState();

  SMTExpr ResExpr = Fctry.createEmptySMTExpr();

  auto Val = V.getValue();
  if (Val.isa<GuardedValueFlowNodeValue>()) {
    ResExpr = GraphSolver->getOrInsertExpr(
        Val.getAs<GuardedValueFlowNodeValue>()->getNode());
  } else {
    std::string Symbol = Val.getAs<AuxValue>()->getName();
    Type *ValTy = Val.getType();
    uint64_t ValTySize = (ValTy && ValTy->isSized()) ? DL.getTypeSizeInBits(ValTy) : 0;
    // could happen for e.g., %struct.kwset_t = type opaque
    if (!ValTySize) {
      ValTySize = 8;
    }
    ResExpr = Fctry.createBitVecConst(Symbol, ValTySize);
  }

  updateUsedCSOuts();
  return ResExpr;
}

SMTExpr PathCondSolver::renameExpr(const SMTExpr &Constr,
                                   const std::string &Suffix) {
  std::unordered_map<std::string, SMTExpr> OldSymNewExpr;

  SMTExprVec TempVec = Fctry.createEmptySMTExprVec();
  TempVec.push_back(Constr);
  return Fctry.rename(TempVec, Suffix, OldSymNewExpr).first.toAndExpr();
}

SMTExpr PathCondSolver::buildBoolVal(bool V) { return Fctry.createBoolVal(V); }

SMTExpr PathCondSolver::buildBoolUnknown(const std::string &Name) {
  return Fctry.createBoolConst(Name);
}

SMTExpr PathCondSolver::buildBitVecVal(uint64_t V, uint64_t Sz) {
  return Fctry.createBitVecVal(V, Sz);
}

SMTExpr PathCondSolver::buildPredicate(const PropertyValue *Va1,
                                       const PropertyValue *Va2,
                                       unsigned Pred) {
  assert(Pred >= CmpInst::ICMP_EQ && Pred <= CmpInst::ICMP_SLE);

  SMTExpr Va1Expr = buildExprForVal(Va1);
  SMTExpr Va2Expr = buildExprForVal(Va2);
  normalizeExprWidth(Va1Expr, Va2Expr, shouldUseSignExtension(Pred));

  static std::function<SMTExpr(SMTExpr &, SMTExpr &)> eqPred =
      [](SMTExpr &Va1Expr, SMTExpr &Va2Expr) { return Va1Expr == Va2Expr; };

  static std::function<SMTExpr(SMTExpr &, SMTExpr &)> neqPred =
      [](SMTExpr &Va1Expr, SMTExpr &Va2Expr) { return Va1Expr != Va2Expr; };

#define GET_PRED(K)                                                            \
  std::function<SMTExpr(SMTExpr &, SMTExpr &)>(                                \
      [](SMTExpr &Va1Expr, SMTExpr &Va2Expr) {                                 \
        return Va1Expr.basic_##K(Va2Expr);                                     \
      })

  static std::map<unsigned, std::function<SMTExpr(SMTExpr &, SMTExpr &)>>
      PredDispatcher = {
          {CmpInst::ICMP_EQ, eqPred},
          {CmpInst::ICMP_NE, neqPred},
          {CmpInst::ICMP_SGE, GET_PRED(sge)},
          {CmpInst::ICMP_SGT, GET_PRED(sgt)},
          {CmpInst::ICMP_SLE, GET_PRED(sle)},
          {CmpInst::ICMP_SLT, GET_PRED(slt)},
          {CmpInst::ICMP_UGE, GET_PRED(uge)},
          {CmpInst::ICMP_UGT, GET_PRED(ugt)},
          {CmpInst::ICMP_ULE, GET_PRED(ule)},
          {CmpInst::ICMP_ULT, GET_PRED(ult)},
      };

  return PredDispatcher.at(Pred)(Va1Expr, Va2Expr);
}

SMTExpr PathCondSolver::buildEqualCond(Var V, const PropertyValue *Va,
                                       const std::string &VSuffix) {
  SMTExpr VExpr = getExpr(V);

  if (VSuffix != "") {
    VExpr = renameExpr(VExpr, VSuffix);
  }

  SMTExpr VaExpr = buildExprForVal(Va);
  normalizeExprWidth(VExpr, VaExpr);

  return (VExpr == VaExpr);
}

SMTExpr PathCondSolver::buildExprForVal(const PropertyValue *Val) {
  if (isa<PropertyInteger>(Val)) {
    return buildIntExpr(cast<PropertyInteger>(Val)->getVal());
  } else {
    return buildSymExpr(cast<PropertySymExpr>(Val));
  }
}

SMTExpr
PathCondSolver::getPhiGated(const GuardedValueFlowPhiNode *PhiNode,
                            const GuardedValueFlowPhiNode::Incoming InNode) {
  resetSolverState();
  auto Res = GraphSolver->getPhiGated(PhiNode, InNode).toAndExpr();
  updateUsedCSOuts();
  return Res;
}

SMTExpr PathCondSolver::getCtrlDeps(BasicBlock *B,
                                    const GuardedValueFlowGraph *G) {
  resetSolverState();
  auto Res = GraphSolver->getCtrlDeps(B, G).toAndExpr();
  updateUsedCSOuts();
  return Res;
}

SMTExpr PathCondSolver::getDataDeps(const GuardedValueFlowNode *N) {
  resetSolverState();
  auto Res = GraphSolver->getDataDeps(N).toAndExpr();
  updateUsedCSOuts();
  return Res;
}

SMTExpr PathCondSolver::buildIntExpr(const BigInteger &I) {
  APInt IntVal = I.getVal();
  return Fctry.createBitVecVal(llvm::toString(IntVal, 10, false),
                               IntVal.getBitWidth());
}

void PathCondSolver::resetSolverState() {
  GraphSolver->reset();
  UsedCSOuts.clear();
}

void PathCondSolver::updateUsedCSOuts() {
  auto IterPair = GraphSolver->getUsedCallSiteOutputs();
  for (auto Iter = IterPair.first; Iter != IterPair.second; ++Iter) {
    auto *CSO = *Iter;
    UsedCSOuts.insert(CSO);
  }
}

void PathCondSolver::normalizeExprWidth(SMTExpr &L, SMTExpr &R,
                                        bool SignExtend) {
  if (L.getBitVecSize() == R.getBitVecSize()) {
    return;
  }

  if (L.getBitVecSize() < R.getBitVecSize()) {
    auto ExtWidth = R.getBitVecSize() - L.getBitVecSize();
    extendExprWidth(L, ExtWidth, SignExtend);
  } else if (L.getBitVecSize() > R.getBitVecSize()) {
    auto ExtWidth = L.getBitVecSize() - R.getBitVecSize();
    extendExprWidth(R, ExtWidth, SignExtend);
  }

  assert(L.getBitVecSize() == R.getBitVecSize());
}

void PathCondSolver::normalizeExprWidth(SMTExpr &E1, SMTExpr &E2, SMTExpr &E3,
                                        bool SignExtend) {
  unsigned W1 = E1.getBitVecSize();
  unsigned W2 = E2.getBitVecSize();
  unsigned W3 = E3.getBitVecSize();

  unsigned MaxWidth = std::max(std::max(W1, W2), W3);

  auto normalizeToWidth = [SignExtend](SMTExpr &E, unsigned W) {
    if (E.getBitVecSize() < W) {
      auto ExtWidth = W - E.getBitVecSize();
      return SignExtend ? E.array_sext(ExtWidth, 1) : E.array_zext(ExtWidth, 1);
    } else {
      assert(E.getBitVecSize() == W);
      return E;
    }
  };

  E1 = normalizeToWidth(E1, MaxWidth);
  E2 = normalizeToWidth(E2, MaxWidth);
  E3 = normalizeToWidth(E3, MaxWidth);
}

SMTExpr PathCondSolver::buildSymExpr(const PropertySymExpr *SymExpr) {
  assert(!SymExpr->isConstant());

  SMTExpr ResExpr = Fctry.createEmptySMTExpr();
  bool hasValue = false;

  auto binAddMul = [this](SMTExpr lhs, SMTExpr rhs, bool isAdd) {
    normalizeExprWidth(lhs, rhs);
    return isAdd ? (lhs + rhs) : (lhs * rhs);
  };

  auto binAdd = [&binAddMul](SMTExpr lhs, SMTExpr rhs) {
    return binAddMul(lhs, rhs, true);
  };

  auto binMul = [&binAddMul](SMTExpr lhs, SMTExpr rhs) {
    return binAddMul(lhs, rhs, false);
  };

  for (const auto &VarCoeff : *SymExpr) {
    Var Va = VarCoeff.first;
    BigInteger Coeff = VarCoeff.second;

    if (Coeff == 1) {
      ResExpr = hasValue ? binAdd(ResExpr, getExpr(Va)) : getExpr(Va);
    } else {
      SMTExpr Term = binMul(buildIntExpr(Coeff), getExpr(Va));
      ResExpr = hasValue ? binAdd(ResExpr, Term) : Term;
    }

    if (!hasValue) {
      hasValue = true;
    }
  }

  assert(hasValue);
  BigInteger Off = SymExpr->getOffsets();
  if (Off != 0) {
    ResExpr = binAdd(ResExpr, buildIntExpr(Off));
  }

  return ResExpr;
}

bool PathCondSolver::isConstraintSat(const SMTExprVec &Cons) {
  GraphSolver->push();

  GraphSolver->addAll(Cons);
  auto Res = GraphSolver->check();

  GraphSolver->pop();

  if (Res == SMTSolver::SMTRT_Unknown || Res == SMTSolver::SMTRT_Uncheck) {
    llvm::errs() << "Oops! Solver fails, the result is "
                 << ((Res == SMTSolver::SMTRT_Unknown) ? "Unknown" : "Uncheck")
                 << "\n";
    return true;
  }

  return Res == SMTSolver::SMTRT_Sat;
}

std::vector<bool> PathCondSolver::batchCheckPredicates(
    const SMTExprVec &Context, const std::vector<SMTExprVec> &Predicates) {
  std::vector<bool> Results;
  Results.reserve(Predicates.size());

  if (Predicates.empty()) {
    return Results;
  }

  // Push the shared context once
  GraphSolver->push();
  GraphSolver->addAll(Context);

  // Check each predicate incrementally
  for (const auto &Pred : Predicates) {
    // Push a new scope for this predicate
    GraphSolver->push();
    GraphSolver->addAll(Pred);

    auto Res = GraphSolver->check();

    // Pop the predicate scope
    GraphSolver->pop();

    if (Res == SMTSolver::SMTRT_Unknown || Res == SMTSolver::SMTRT_Uncheck) {
      llvm::errs() << "Warning: Solver returned "
                   << ((Res == SMTSolver::SMTRT_Unknown) ? "Unknown"
                                                         : "Uncheck")
                   << " for batch predicate check\n";
      // Conservative: assume satisfiable if solver fails
      Results.push_back(true);
    } else {
      Results.push_back(Res == SMTSolver::SMTRT_Sat);
    }
  }

  // Pop the context scope
  GraphSolver->pop();

  return Results;
}

void PathCondSolver::batchCheckPredicatesWithAnalysis(
    const SMTExprVec &Context, const std::vector<SMTExprVec> &Predicates,
    std::vector<bool> &Results, unsigned &PrunedCount) {
  Results.clear();
  Results.resize(Predicates.size(), false);
  PrunedCount = 0;

  if (Predicates.empty()) {
    return;
  }

  // Simple analysis: check if context itself is unsatisfiable
  // If so, all predicates are unsatisfiable
  GraphSolver->push();
  GraphSolver->addAll(Context);
  auto ContextRes = GraphSolver->check();
  GraphSolver->pop();

  if (ContextRes == SMTSolver::SMTRT_Unsat) {
    // Context is unsatisfiable, so all predicates are unsatisfiable
    PrunedCount = Predicates.size();
    return;
  }

  if (ContextRes == SMTSolver::SMTRT_Unknown ||
      ContextRes == SMTSolver::SMTRT_Uncheck) {
    // If context check fails, fall back to regular batch checking
    Results = batchCheckPredicates(Context, Predicates);
    return;
  }

  // Context is satisfiable, proceed with batch checking
  // Push the shared context once
  GraphSolver->push();
  GraphSolver->addAll(Context);

  // Check each predicate incrementally
  for (size_t i = 0; i < Predicates.size(); ++i) {
    const auto &Pred = Predicates[i];

    // Simple pruning: check if predicate is trivially false
    // (This is a placeholder - more sophisticated analysis can be added)
    bool ShouldCheck = true;

    if (ShouldCheck) {
      // Push a new scope for this predicate
      GraphSolver->push();
      GraphSolver->addAll(Pred);

      auto Res = GraphSolver->check();

      // Pop the predicate scope
      GraphSolver->pop();

      if (Res == SMTSolver::SMTRT_Unknown || Res == SMTSolver::SMTRT_Uncheck) {
        llvm::errs() << "Warning: Solver returned "
                     << ((Res == SMTSolver::SMTRT_Unknown) ? "Unknown"
                                                           : "Uncheck")
                     << " for batch predicate check\n";
        // Conservative: assume satisfiable if solver fails
        Results[i] = true;
      } else {
        Results[i] = (Res == SMTSolver::SMTRT_Sat);
      }
    } else {
      // Predicate was pruned
      Results[i] = false;
      ++PrunedCount;
    }
  }

  // Pop the context scope
  GraphSolver->pop();
}

unsigned PathCondSolver::getExprSize(const SMTExpr &E, unsigned LimitVal) {
  auto EID = E.getAstId();
  if (ConstrToSize.count(EID)) {
    return ConstrToSize.at(EID);
  }

  unsigned ExprSize = 0;

  std::vector<SMTExpr> ST;
  std::unordered_map<unsigned, SMTExpr> Visited;
  ST.emplace_back(E);
  Visited.insert(std::make_pair(EID, E));
  while (!ST.empty()) {
    auto CurE = ST.back();
    ST.pop_back();

    if (CurE.isApp()) {
      for (unsigned I = 0, Num = CurE.numArgs(); I < Num; I++) {
        auto NextE = CurE.getArg(I);
        auto Id = NextE.getAstId();

        if (!Visited.count(Id)) {
          ST.emplace_back(NextE);
          Visited.insert(std::make_pair(Id, NextE));
        }
      }
    }

    if (Visited.size() >= LimitVal) {
      break;
    }
  }

  ExprSize = Visited.size();
  ConstrToSize[EID] = ExprSize;
  return ExprSize;
}
