#include "SymbolicExecution/Solver/PathCondSolver.h"

#include "Solvers/SMT/LIBSMT/SMTModel.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <unordered_map>
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

bool modelSatisfiesExprs(const SMTModel &Model,
                         const SmallVectorImpl<SMTExpr> &Exprs) {
  try {
    for (const SMTExpr &E : Exprs) {
      if (!Model.Eval(E, /*ModelCompletion=*/true).isTrue()) {
        return false;
      }
    }
    return true;
  } catch (const z3::exception &) {
    return false;
  }
}

void flattenAndExprs(const SMTExprVec &Vec, std::vector<SMTExpr> &Out,
                     bool &HasFalse) {
  HasFalse = false;
  llvm::DenseSet<unsigned> Seen;
  std::vector<SMTExpr> Stack;
  Stack.reserve(Vec.size());
  for (unsigned I = Vec.size(); I > 0; --I) {
    Stack.push_back(Vec[I - 1]);
  }
  while (!Stack.empty()) {
    SMTExpr Cur = std::move(Stack.back());
    Stack.pop_back();
    if (!Cur.isApp()) {
      if (Seen.insert(Cur.getAstId()).second) {
        Out.push_back(std::move(Cur));
      }
      continue;
    }
    if (Cur.isFalse()) {
      HasFalse = true;
      Out.clear();
      return;
    }
    if (Cur.isTrue()) {
      continue;
    }
    if (Cur.isLogicAnd()) {
      unsigned N = Cur.numArgs();
      for (unsigned I = N; I > 0; --I) {
        Stack.push_back(Cur.getArg(I - 1));
      }
      continue;
    }
    if (Seen.insert(Cur.getAstId()).second) {
      Out.push_back(std::move(Cur));
    }
  }
}

bool containsDivOrRem(const SMTExpr &E) {
  llvm::DenseSet<unsigned> Visited;
  std::vector<SMTExpr> Stack;
  Stack.push_back(E);
  Visited.insert(E.getAstId());
  while (!Stack.empty()) {
    SMTExpr Cur = std::move(Stack.back());
    Stack.pop_back();
    if (!Cur.isApp()) {
      continue;
    }
    Z3_decl_kind DK = Z3_get_decl_kind(Cur.getZ3Expr().ctx(), Cur.getZ3Expr().decl());
    if (DK == Z3_OP_BSDIV || DK == Z3_OP_BUDIV || DK == Z3_OP_BSREM ||
        DK == Z3_OP_BUREM || DK == Z3_OP_BSMOD || DK == Z3_OP_BSDIV_I ||
        DK == Z3_OP_BUDIV_I || DK == Z3_OP_BSREM_I || DK == Z3_OP_BUREM_I) {
      return true;
    }
    for (unsigned I = 0, N = Cur.numArgs(); I < N; ++I) {
      SMTExpr Arg = Cur.getArg(I);
      if (Visited.insert(Arg.getAstId()).second) {
        Stack.push_back(std::move(Arg));
      }
    }
  }
  return false;
}

bool isSubsetSorted(const SmallVectorImpl<unsigned> &Sub,
                    const SmallVectorImpl<unsigned> &Sup) {
  return std::includes(Sup.begin(), Sup.end(), Sub.begin(), Sub.end());
}

} // namespace

std::atomic<uint32_t> PathCondSolver::ID{0};

PathCondSolver::PathCondSolver(const llvm::DataLayout &DL)
    : DL(DL), SolverID(ID++) {}

GuardedValueFlowSolver &PathCondSolver::getGraphSolver() {
  if (!GraphSolver) {
    GraphSolver = std::make_unique<GuardedValueFlowSolver>(Fctry, DL);
  }
  return *GraphSolver;
}

SMTSolver &PathCondSolver::getQuerySolver() {
  if (!QuerySolver) {
    QuerySolver =
        std::make_unique<SMTSolver>(Fctry.createSMTSolverWithTactic("qfbv"));
  }
  return *QuerySolver;
}

const std::vector<SMTExpr> &PathCondSolver::getFreeConsts(const SMTExpr &E) {
  unsigned RootId = E.getAstId();
  auto It = ExprFreeConstsCache.find(RootId);
  if (It != ExprFreeConstsCache.end()) {
    return It->second;
  }

  std::vector<SMTExpr> Consts;
  llvm::DenseSet<unsigned> Visited;
  std::vector<SMTExpr> Stack;
  Stack.push_back(E);
  Visited.insert(RootId);

  while (!Stack.empty()) {
    SMTExpr Cur = std::move(Stack.back());
    Stack.pop_back();
    if (!Cur.isApp()) {
      continue;
    }
    unsigned N = Cur.numArgs();
    if (N == 0) {
      if (!Cur.isNumeral() && !Cur.isTrue() && !Cur.isFalse() &&
          Cur.isConst()) {
        Consts.push_back(std::move(Cur));
      }
      continue;
    }
    for (unsigned I = 0; I < N; ++I) {
      SMTExpr Arg = Cur.getArg(I);
      if (Visited.insert(Arg.getAstId()).second) {
        Stack.push_back(std::move(Arg));
      }
    }
  }

  auto Ins = ExprFreeConstsCache.emplace(RootId, std::move(Consts));
  return Ins.first->second;
}

SMTExpr PathCondSolver::buildRegionCondition(GuardedValueFlowRegionNode *Cond) {
  UsedCSOuts.clear();
  auto &GS = getGraphSolver();
  GS.push();

  auto Res = GS.getDataDeps(Cond);
  Res.push_back(GS.getOrInsertExpr(Cond).bv12bool());
  updateUsedCSOuts();
  GS.pop();

  return Res.toAndExpr();
}

SMTExprVec PathCondSolver::createEmptySMTExprVec() {
  return Fctry.createEmptySMTExprVec();
}

SMTExpr PathCondSolver::getExpr(Var V) {
  UsedCSOuts.clear();
  auto &GS = getGraphSolver();
  GS.push();

  SMTExpr ResExpr = Fctry.createEmptySMTExpr();

  auto Val = V.getValue();
  if (Val.isVacuous()) {
    ResExpr = Fctry.createBitVecVal(0, 8);
  } else if (Val.isa<GuardedValueFlowNodeValue>()) {
    ResExpr =
        GS.getOrInsertExpr(Val.getAs<GuardedValueFlowNodeValue>()->getNode());
  } else {
    std::string Symbol = Val.getAs<AuxValue>()->getName();
    Type *ValTy = Val.getType();
    uint64_t ValTySize =
        (ValTy && ValTy->isSized()) ? DL.getTypeSizeInBits(ValTy) : 0;
    // could happen for e.g., %struct.kwset_t = type opaque
    if (!ValTySize) {
      ValTySize = 8;
    }
    ResExpr = Fctry.createBitVecConst(Symbol, ValTySize);
  }

  updateUsedCSOuts();
  GS.pop();
  return ResExpr;
}

SMTExpr PathCondSolver::renameExpr(const SMTExpr &Constr,
                                   const std::string &Suffix) {
  if (Suffix.empty()) {
    return Constr;
  }
  if (Constr.isApp() && Constr.numArgs() == 0) {
    if (Constr.isNumeral() || Constr.isTrue() || Constr.isFalse()) {
      return Constr;
    }
    if (Constr.isConst()) {
      SMTExpr Copy(Constr);
      std::string NewSymbol = Copy.getSymbol() + Suffix;
      if (Copy.isBitVector()) {
        return Fctry.createBitVecConst(NewSymbol, Copy.getBitVecSize());
      }
      if (Copy.isBool()) {
        return Fctry.createBoolConst(NewSymbol);
      }
      if (Copy.isReal()) {
        return Fctry.createRealConst(NewSymbol);
      }
    }
  }

  const auto &Consts = getFreeConsts(Constr);
  if (Consts.empty()) {
    return Constr;
  }

  SMTExprVec From = Fctry.createEmptySMTExprVec();
  SMTExprVec To = Fctry.createEmptySMTExprVec();
  bool UnsupportedSort = false;
  for (const SMTExpr &OldConst : Consts) {
    SMTExpr Copy(OldConst);
    std::string NewSymbol = Copy.getSymbol() + Suffix;
    if (Copy.isBitVector()) {
      From.push_back(Copy);
      To.push_back(Fctry.createBitVecConst(NewSymbol, Copy.getBitVecSize()));
    } else if (Copy.isBool()) {
      From.push_back(Copy);
      To.push_back(Fctry.createBoolConst(NewSymbol));
    } else if (Copy.isReal()) {
      From.push_back(Copy);
      To.push_back(Fctry.createRealConst(NewSymbol));
    } else {
      UnsupportedSort = true;
      break;
    }
  }

  if (!UnsupportedSort) {
    if (From.empty()) {
      return Constr;
    }
    SMTExpr Copy(Constr);
    return Copy.substitute(From, To);
  }

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

  switch (Pred) {
  case CmpInst::ICMP_EQ:
    return Va1Expr == Va2Expr;
  case CmpInst::ICMP_NE:
    return Va1Expr != Va2Expr;
  case CmpInst::ICMP_SGE:
    return Va1Expr.basic_sge(Va2Expr);
  case CmpInst::ICMP_SGT:
    return Va1Expr.basic_sgt(Va2Expr);
  case CmpInst::ICMP_SLE:
    return Va1Expr.basic_sle(Va2Expr);
  case CmpInst::ICMP_SLT:
    return Va1Expr.basic_slt(Va2Expr);
  case CmpInst::ICMP_UGE:
    return Va1Expr.basic_uge(Va2Expr);
  case CmpInst::ICMP_UGT:
    return Va1Expr.basic_ugt(Va2Expr);
  case CmpInst::ICMP_ULE:
    return Va1Expr.basic_ule(Va2Expr);
  case CmpInst::ICMP_ULT:
    return Va1Expr.basic_ult(Va2Expr);
  default:
    return buildBoolVal(true);
  }
}

SMTExpr PathCondSolver::buildEqualCond(Var V, const PropertyValue *Va,
                                       const std::string &VSuffix) {
  SMTExpr VExpr = getExpr(V);

  if (!VSuffix.empty()) {
    VExpr = renameExpr(VExpr, VSuffix);
  }

  SMTExpr VaExpr = buildExprForVal(Va);
  normalizeExprWidth(VExpr, VaExpr);

  return (VExpr == VaExpr);
}

SMTExpr PathCondSolver::buildExprForVal(const PropertyValue *Val) {
  if (!Val) {
    return buildBitVecVal(0, 8);
  }
  if (isa<PropertyInteger>(Val)) {
    return buildIntExpr(cast<PropertyInteger>(Val)->getVal());
  }
  return buildSymExpr(cast<PropertySymExpr>(Val));
}

SMTExpr
PathCondSolver::getPhiGated(const GuardedValueFlowPhiNode *PhiNode,
                            const GuardedValueFlowPhiNode::Incoming InNode) {
  UsedCSOuts.clear();
  auto &GS = getGraphSolver();
  GS.push();
  auto Res = GS.getPhiGated(PhiNode, InNode).toAndExpr();
  updateUsedCSOuts();
  GS.pop();
  return Res;
}

SMTExpr PathCondSolver::getCtrlDeps(BasicBlock *B,
                                    const GuardedValueFlowGraph *G) {
  UsedCSOuts.clear();
  auto &GS = getGraphSolver();
  GS.push();
  auto Res = GS.getCtrlDeps(B, G).toAndExpr();
  updateUsedCSOuts();
  GS.pop();
  return Res;
}

SMTExpr PathCondSolver::getDataDeps(const GuardedValueFlowNode *N) {
  UsedCSOuts.clear();
  auto &GS = getGraphSolver();
  GS.push();
  auto Res = GS.getDataDeps(N).toAndExpr();
  updateUsedCSOuts();
  GS.pop();
  return Res;
}

SMTExpr PathCondSolver::buildIntExpr(const BigInteger &I) {
  APInt IntVal = I.getVal();
  return Fctry.createBitVecVal(llvm::toString(IntVal, 10, false),
                               IntVal.getBitWidth());
}

void PathCondSolver::resetSolverState() {
  if (GraphSolver) {
    GraphSolver->reset();
  }
  UsedCSOuts.clear();
}

void PathCondSolver::updateUsedCSOuts() {
  if (!GraphSolver) {
    return;
  }
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

SMTExpr PathCondSolver::makeConstMul(SMTExpr Val, uint64_t C) {
  uint64_t W = Val.getBitVecSize();
  if (C == 0) {
    return Fctry.createBitVecVal(0, W);
  }
  uint64_t TZ = llvm::countTrailingZeros(C);
  if (TZ >= W) {
    return Fctry.createBitVecVal(0, W);
  }

  auto makeConstShl = [this](SMTExpr &V, uint64_t Shift) -> SMTExpr {
    uint64_t VW = V.getBitVecSize();
    if (Shift == 0) {
      return V;
    }
    if (Shift >= VW) {
      return Fctry.createBitVecVal(0, VW);
    }
    SMTExpr LowZeros = Fctry.createBitVecVal(0, Shift);
    return V.basic_extract(static_cast<unsigned>(VW - 1 - Shift), 0)
        .basic_concat(LowZeros);
  };

  uint64_t Odd = C >> TZ;
  if (Odd == 1) {
    return makeConstShl(Val, TZ);
  }

  // Exact Non-Adjacent Form (NAF) shift-add lowering matching
  // GuardedValueFlowSolver::encodeBinaryOpcodeNode so Z3 shares sub-DAGs
  // and avoids WxW bit-blasted multipliers for constant coefficients.
  SmallVector<std::pair<unsigned, int>, 10> Terms;
  __uint128_t V = Odd;
  unsigned Bit = 0;
  while (V != 0) {
    if (V & 1) {
      int Digit = 2 - static_cast<int>(V & 3);
      if (Bit + TZ < W) {
        if (Terms.size() >= 8) {
          return Fctry.createBitVecVal(C, W) * Val;
        }
        Terms.push_back({Bit, Digit});
      }
      if (Digit > 0) {
        V -= 1;
      } else {
        V += 1;
      }
    }
    V >>= 1;
    ++Bit;
  }

  if (Terms.empty()) {
    return Fctry.createBitVecVal(0, W);
  }

  SMTExpr Acc = Fctry.createEmptySMTExpr();
  bool HasAcc = false;
  for (auto It = Terms.rbegin(), E = Terms.rend(); It != E; ++It) {
    SMTExpr Term = makeConstShl(Val, It->first);
    if (!HasAcc) {
      Acc = (It->second > 0)
                ? Term
                : Fctry.createBitVecVal(0, W).basic_sub(Term);
      HasAcc = true;
    } else {
      Acc = (It->second > 0) ? Acc.basic_add(Term) : Acc.basic_sub(Term);
    }
  }
  return makeConstShl(Acc, TZ);
}

SMTExpr PathCondSolver::buildSymExpr(const PropertySymExpr *SymExpr) {
  if (!SymExpr || SymExpr->isConstant()) {
    return SymExpr ? buildIntExpr(SymExpr->getAsConstant())
                   : buildBitVecVal(0, 8);
  }

  SMTExpr ResExpr = Fctry.createEmptySMTExpr();
  bool hasValue = false;

  auto binAdd = [this](SMTExpr lhs, SMTExpr rhs) {
    normalizeExprWidth(lhs, rhs);
    return lhs + rhs;
  };

  auto binSub = [this](SMTExpr lhs, SMTExpr rhs) {
    normalizeExprWidth(lhs, rhs);
    return lhs - rhs;
  };

  auto binMul = [this](SMTExpr lhs, SMTExpr rhs) {
    normalizeExprWidth(lhs, rhs);
    return lhs * rhs;
  };

  for (const auto &VarCoeff : *SymExpr) {
    Var Va = VarCoeff.first;
    BigInteger Coeff = VarCoeff.second;

    if (Coeff == 1) {
      SMTExpr VarE = getExpr(Va);
      ResExpr = hasValue ? binAdd(ResExpr, VarE) : VarE;
    } else if (Coeff == -1) {
      SMTExpr VarE = getExpr(Va);
      if (hasValue) {
        ResExpr = binSub(ResExpr, VarE);
      } else {
        ResExpr = Fctry.createBitVecVal(0, VarE.getBitVecSize()).basic_sub(VarE);
      }
    } else {
      SMTExpr VarE = getExpr(Va);
      APInt CoeffVal = Coeff.getVal();
      if (CoeffVal.getMinSignedBits() <= 64) {
        int64_t SC = CoeffVal.getSExtValue();
        bool IsNeg = SC < 0;
        uint64_t AbsC = IsNeg ? (static_cast<uint64_t>(-(SC + 1)) + 1ULL)
                              : static_cast<uint64_t>(SC);
        unsigned CoeffW = CoeffVal.getBitWidth();
        if (VarE.getBitVecSize() < CoeffW) {
          extendExprWidth(VarE, CoeffW - VarE.getBitVecSize(), true);
        }
        SMTExpr Prod = makeConstMul(VarE, AbsC);
        if (IsNeg) {
          if (hasValue) {
            ResExpr = binSub(ResExpr, Prod);
          } else {
            ResExpr =
                Fctry.createBitVecVal(0, Prod.getBitVecSize()).basic_sub(Prod);
          }
        } else {
          ResExpr = hasValue ? binAdd(ResExpr, Prod) : Prod;
        }
      } else {
        SMTExpr Term = binMul(buildIntExpr(Coeff), VarE);
        ResExpr = hasValue ? binAdd(ResExpr, Term) : Term;
      }
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
  std::vector<SMTExpr> Flat;
  bool HasFalse = false;
  flattenAndExprs(Cons, Flat, HasFalse);
  if (HasFalse) {
    return false;
  }
  if (Flat.empty()) {
    return true;
  }

  auto &QS = getQuerySolver();
  QS.push();
  QS.addAll(Flat);
  auto Res = QS.check();
  QS.pop();

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
  std::vector<bool> Results(Predicates.size(), false);
  if (Predicates.empty()) {
    return Results;
  }

  // Flatten and deduplicate top-level conjuncts of Context.
  std::vector<SMTExpr> CtxConjuncts;
  bool CtxHasFalse = false;
  flattenAndExprs(Context, CtxConjuncts, CtxHasFalse);
  if (CtxHasFalse) {
    return Results;
  }
  const bool HasNonTrueContext = !CtxConjuncts.empty();

  // Deduplicate predicates and separate trivially true/false predicates.
  struct UniquePred {
    SmallVector<SMTExpr, 2> Exprs;
    SmallVector<size_t, 2> Owners;
    SmallVector<unsigned, 4> CompRoots;
    bool Resolved = false;
    bool Sat = false;
  };

  std::vector<size_t> EmptyOwners;
  std::vector<UniquePred> UniquePreds;
  std::unordered_map<unsigned, size_t> SingleAstToUnique;
  std::unordered_map<std::string, size_t> MultiAstToUnique;

  for (size_t Idx = 0; Idx < Predicates.size(); ++Idx) {
    const auto &Pred = Predicates[Idx];
    std::vector<SMTExpr> FlatPred;
    bool HasFalse = false;
    flattenAndExprs(Pred, FlatPred, HasFalse);

    if (HasFalse) {
      Results[Idx] = false;
      continue;
    }
    if (FlatPred.empty()) {
      EmptyOwners.push_back(Idx);
      continue;
    }

    SmallVector<SMTExpr, 2> NonTrue(FlatPred.begin(), FlatPred.end());
    if (NonTrue.size() == 1) {
      unsigned AstId = NonTrue[0].getAstId();
      auto Ins = SingleAstToUnique.emplace(AstId, UniquePreds.size());
      if (Ins.second) {
        UniquePred UP;
        UP.Exprs = std::move(NonTrue);
        UP.Owners.push_back(Idx);
        UniquePreds.push_back(std::move(UP));
      } else {
        UniquePreds[Ins.first->second].Owners.push_back(Idx);
      }
    } else {
      SmallVector<unsigned, 4> Ids;
      Ids.reserve(NonTrue.size());
      for (const SMTExpr &E : NonTrue) {
        Ids.push_back(E.getAstId());
      }
      std::sort(Ids.begin(), Ids.end());
      std::string Key(reinterpret_cast<const char *>(Ids.data()),
                      Ids.size() * sizeof(unsigned));
      auto Ins = MultiAstToUnique.emplace(std::move(Key), UniquePreds.size());
      if (Ins.second) {
        UniquePred UP;
        UP.Exprs = std::move(NonTrue);
        UP.Owners.push_back(Idx);
        UniquePreds.push_back(std::move(UP));
      } else {
        UniquePreds[Ins.first->second].Owners.push_back(Idx);
      }
    }
  }

  auto MarkPred = [&](UniquePred &UP, bool IsSat) {
    UP.Resolved = true;
    UP.Sat = IsSat;
    for (size_t Owner : UP.Owners) {
      Results[Owner] = IsSat;
    }
  };

  auto &QS = getQuerySolver();

  // Step 1: Check shared Context once at Z3 scope level 0 using tactic("qfbv").
  // If UNSAT, all predicates are UNSAT. If SAT, harvest its model to witness
  // satisfiable predicates with zero extra Z3 calls.
  bool ContextKnownSat = !HasNonTrueContext;
  if (HasNonTrueContext) {
    QS.push();
    QS.addAll(CtxConjuncts);
    auto CtxRes = QS.check();

    if (CtxRes == SMTSolver::SMTRT_Unsat) {
      QS.pop();
      return Results;
    }

    if (CtxRes == SMTSolver::SMTRT_Sat) {
      ContextKnownSat = true;
      for (size_t Owner : EmptyOwners) {
        Results[Owner] = true;
      }
      if (!UniquePreds.empty()) {
        SMTModel CtxModel = QS.getSMTModel();
        for (auto &UP : UniquePreds) {
          if (!UP.Resolved && modelSatisfiesExprs(CtxModel, UP.Exprs)) {
            MarkPred(UP, true);
          }
        }
      }
    } else {
      for (size_t Owner : EmptyOwners) {
        Results[Owner] = true;
      }
    }

    QS.pop();
  } else {
    for (size_t Owner : EmptyOwners) {
      Results[Owner] = true;
    }
  }

  size_t UnresolvedCount = 0;
  for (const auto &UP : UniquePreds) {
    if (!UP.Resolved) {
      ++UnresolvedCount;
    }
  }
  if (UnresolvedCount == 0) {
    return Results;
  }

  // Step 2: When Context is known SAT, partition CtxConjuncts into exact
  // variable-connected components. Since Context = Rel(P) /\ Rest(P) with
  // disjoint variables and Rest(P) is SAT, Context /\ P is SAT iff Rel(P) /\ P
  // is SAT.
  std::vector<unsigned> UF;
  auto FindRoot = [&](unsigned X) -> unsigned {
    while (UF[X] != X) {
      UF[X] = UF[UF[X]];
      X = UF[X];
    }
    return X;
  };
  auto Unite = [&](unsigned A, unsigned B) {
    unsigned RA = FindRoot(A);
    unsigned RB = FindRoot(B);
    if (RA != RB) {
      UF[RB] = RA;
    }
  };

  std::unordered_map<unsigned, unsigned> VarToConj;
  std::unordered_map<unsigned, std::vector<size_t>> RootToConjIndices;
  std::vector<bool> ConjHasDivRem;

  if (HasNonTrueContext && ContextKnownSat) {
    const size_t NumConj = CtxConjuncts.size();
    UF.resize(NumConj);
    ConjHasDivRem.resize(NumConj, false);
    for (size_t CIdx = 0; CIdx < NumConj; ++CIdx) {
      UF[CIdx] = static_cast<unsigned>(CIdx);
      const auto &Vars = getFreeConsts(CtxConjuncts[CIdx]);
      for (const SMTExpr &V : Vars) {
        unsigned VId = V.getAstId();
        auto Ins = VarToConj.emplace(VId, static_cast<unsigned>(CIdx));
        if (!Ins.second) {
          Unite(Ins.first->second, static_cast<unsigned>(CIdx));
        }
      }
    }
    for (size_t CIdx = 0; CIdx < NumConj; ++CIdx) {
      unsigned Root = FindRoot(static_cast<unsigned>(CIdx));
      RootToConjIndices[Root].push_back(CIdx);
      ConjHasDivRem[CIdx] = containsDivOrRem(CtxConjuncts[CIdx]);
    }

    for (auto &UP : UniquePreds) {
      if (UP.Resolved) {
        continue;
      }
      for (const SMTExpr &E : UP.Exprs) {
        for (const SMTExpr &V : getFreeConsts(E)) {
          auto VIt = VarToConj.find(V.getAstId());
          if (VIt != VarToConj.end()) {
            UP.CompRoots.push_back(FindRoot(VIt->second));
          }
        }
      }
      std::sort(UP.CompRoots.begin(), UP.CompRoots.end());
      UP.CompRoots.erase(std::unique(UP.CompRoots.begin(), UP.CompRoots.end()),
                         UP.CompRoots.end());
    }
  }

  // Step 3: Solve each remaining unresolved predicate at Z3 scope level 0 on
  // tactic("qfbv"), using exact COI slicing, optional cheap-subset UNSAT
  // pre-check when div/rem is present, and model harvesting across predicates
  // whose components are covered by the solved slice.
  for (size_t I = 0; I < UniquePreds.size(); ++I) {
    if (UniquePreds[I].Resolved) {
      continue;
    }

    std::vector<SMTExpr> SliceConjuncts;
    std::vector<SMTExpr> CheapConjuncts;
    std::vector<SMTExpr> ExpensiveConjuncts;
    bool SliceHasDivRem = false;

    if (HasNonTrueContext) {
      if (ContextKnownSat) {
        for (unsigned Root : UniquePreds[I].CompRoots) {
          for (size_t CIdx : RootToConjIndices[Root]) {
            SliceConjuncts.push_back(CtxConjuncts[CIdx]);
            if (ConjHasDivRem[CIdx]) {
              SliceHasDivRem = true;
              ExpensiveConjuncts.push_back(CtxConjuncts[CIdx]);
            } else {
              CheapConjuncts.push_back(CtxConjuncts[CIdx]);
            }
          }
        }
      } else {
        SliceConjuncts = CtxConjuncts;
      }
    }

    // Exact subset-UNSAT / model-witness fast path when the slice contains
    // expensive div/rem conjuncts alongside cheap conjuncts.
    if (SliceHasDivRem && !CheapConjuncts.empty()) {
      QS.push();
      QS.addAll(CheapConjuncts);
      for (const SMTExpr &E : UniquePreds[I].Exprs) {
        QS.add(E);
      }
      auto CheapRes = QS.check();
      if (CheapRes == SMTSolver::SMTRT_Unsat) {
        QS.pop();
        MarkPred(UniquePreds[I], false);
        continue;
      }
      if (CheapRes == SMTSolver::SMTRT_Sat) {
        SMTModel CheapModel = QS.getSMTModel();
        QS.pop();
        SmallVector<SMTExpr, 4> ExpVec(ExpensiveConjuncts.begin(),
                                       ExpensiveConjuncts.end());
        if (modelSatisfiesExprs(CheapModel, ExpVec)) {
          MarkPred(UniquePreds[I], true);
          for (size_t J = I + 1; J < UniquePreds.size(); ++J) {
            if (!UniquePreds[J].Resolved &&
                isSubsetSorted(UniquePreds[J].CompRoots,
                               UniquePreds[I].CompRoots) &&
                modelSatisfiesExprs(CheapModel, UniquePreds[J].Exprs)) {
              MarkPred(UniquePreds[J], true);
            }
          }
          continue;
        }
      } else {
        QS.pop();
      }
    }

    QS.push();
    if (!SliceConjuncts.empty()) {
      QS.addAll(SliceConjuncts);
    }
    for (const SMTExpr &E : UniquePreds[I].Exprs) {
      QS.add(E);
    }
    auto Res = QS.check();
    if (Res == SMTSolver::SMTRT_Sat) {
      MarkPred(UniquePreds[I], true);
      SMTModel M = QS.getSMTModel();
      QS.pop();
      for (size_t J = I + 1; J < UniquePreds.size(); ++J) {
        if (UniquePreds[J].Resolved) {
          continue;
        }
        bool Covered = !HasNonTrueContext || !ContextKnownSat ||
                       isSubsetSorted(UniquePreds[J].CompRoots,
                                      UniquePreds[I].CompRoots);
        if (Covered && modelSatisfiesExprs(M, UniquePreds[J].Exprs)) {
          MarkPred(UniquePreds[J], true);
        }
      }
    } else {
      QS.pop();
      if (Res == SMTSolver::SMTRT_Unknown || Res == SMTSolver::SMTRT_Uncheck) {
        llvm::errs() << "Warning: Solver returned "
                     << ((Res == SMTSolver::SMTRT_Unknown) ? "Unknown"
                                                           : "Uncheck")
                     << " for batch predicate check\n";
        MarkPred(UniquePreds[I], true);
      } else {
        MarkPred(UniquePreds[I], false);
      }
    }
  }

  return Results;
}

void PathCondSolver::batchCheckPredicatesWithAnalysis(
    const SMTExprVec &Context, const std::vector<SMTExprVec> &Predicates,
    std::vector<bool> &Results, unsigned &PrunedCount) {
  Results = batchCheckPredicates(Context, Predicates);
  PrunedCount = 0;
  for (bool R : Results) {
    if (!R) {
      ++PrunedCount;
    }
  }
}

unsigned PathCondSolver::getExprSize(const SMTExpr &E, unsigned LimitVal) {
  auto EID = E.getAstId();
  auto CacheIt = ConstrToSize.find(EID);
  if (CacheIt != ConstrToSize.end()) {
    return CacheIt->second;
  }

  std::vector<SMTExpr> ST;
  llvm::DenseSet<unsigned> Visited;
  ST.emplace_back(E);
  Visited.insert(EID);
  while (!ST.empty()) {
    auto CurE = ST.back();
    ST.pop_back();

    if (CurE.isApp()) {
      for (unsigned I = 0, Num = CurE.numArgs(); I < Num; I++) {
        auto NextE = CurE.getArg(I);
        auto Id = NextE.getAstId();

        if (Visited.insert(Id).second) {
          ST.emplace_back(std::move(NextE));
        }
      }
    }

    if (Visited.size() >= LimitVal) {
      break;
    }
  }

  unsigned ExprSize = Visited.size();
  ConstrToSize[EID] = ExprSize;
  return ExprSize;
}
