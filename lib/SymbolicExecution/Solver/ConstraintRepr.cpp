#include "SymbolicExecution/Solver/ConstraintRepr.h"

#include "SymbolicExecution/Solver/PathCondSolver.h"

#include <atomic>
#include <sstream>

using namespace llvm;
using namespace SymbolicExecution;

SMTCondition::SMTCondition(bool V) {
  if (V) {
    Lit = 1;
  } else {
    Lit = 0;
  }

  // Lit is 0 or 1; Lit - 2 is -2 or -1
  Index = Lit - 2;
}

SMTCondition::SMTCondition(GuardedValueFlowRegionNode *Cond,
                           PathCondSolver *Solver) {
  if (Cond && Cond->isAlwaysTrue()) {
    Lit = 1;
    Index = -1;
    return;
  }
  SMTExpr Built = Solver->buildRegionCondition(Cond);
  if (Built.isApp() && Built.isTrue()) {
    Lit = 1;
    Index = -1;
    return;
  }
  if (Built.isApp() && Built.isFalse()) {
    Lit = 0;
    Index = -2;
    return;
  }
  Lit = -1;
  Constr = std::make_unique<SMTExpr>(std::move(Built));
  this->Solver = Solver;
  Index = ((uint64_t)(Solver->getID()) << 32) | this->Constr->getAstId();
}

SMTCondition::SMTCondition(const SMTExpr &Constr, PathCondSolver *Solver) {
  if (Constr.isApp() && Constr.isTrue()) {
    Lit = 1;
    Index = -1;
    return;
  }
  if (Constr.isApp() && Constr.isFalse()) {
    Lit = 0;
    Index = -2;
    return;
  }
  Lit = -1;
  this->Constr = std::make_unique<SMTExpr>(Constr);
  this->Solver = Solver;
  Index = ((uint64_t)(Solver->getID()) << 32) | this->Constr->getAstId();
}

SMTCondition::SMTCondition(PropertyValuePtr L, PropertyValuePtr R,
                           CmpInst::Predicate Pred, PathCondSolver *Solver) {
  SMTExpr Built = Solver->buildPredicate(L.get(), R.get(), Pred);
  if (Built.isApp() && Built.isTrue()) {
    Lit = 1;
    Index = -1;
    return;
  }
  if (Built.isApp() && Built.isFalse()) {
    Lit = 0;
    Index = -2;
    return;
  }
  Lit = -1;
  Constr = std::make_unique<SMTExpr>(std::move(Built));
  this->Solver = Solver;
  Index = ((uint64_t)(Solver->getID()) << 32) | this->Constr->getAstId();
}

bool SMTCondition::isTrue() const {
  if (isLit()) {
    return Lit == 1;
  }

  return Constr->isTrue();
}

bool SMTCondition::isFalse() const {
  if (isLit()) {
    return Lit == 0;
  }

  return Constr->isFalse();
}

void SMTCondition::dump() const {
  if (isLit()) {
    llvm::errs() << (Lit == 0 ? "false" : "true") << "\n";
  } else {
    std::ostringstream os;
    os << *Constr;
    llvm::errs() << os.str() << "\n";
  }
}

SMTCondition SMTCondition::operator||(const SMTCondition &R) const {
  if (isTrue() || R.isFalse()) {
    return *this;
  }

  if (isFalse() || R.isTrue()) {
    return R;
  }

  assert(Solver == R.Solver);
  if (*this == R) {
    return *this;
  }

  auto ResConstr = (*Constr) || (*R.Constr);
  auto Res = SMTCondition(ResConstr, Solver);
  return Res;
}

SMTCondition SMTCondition::operator&&(const SMTCondition &R) const {
  if (isTrue() || R.isFalse()) {
    return R;
  }

  if (isFalse() || R.isTrue()) {
    return *this;
  }

  assert(Solver == R.Solver);
  // necessary to avoid overestimate of formula size. It seems that z3
  // does not perform this simplification internally.
  if (*this == R) {
    return *this;
  }

  auto ResConstr = *Constr && *R.Constr;
  auto Res = SMTCondition(ResConstr, Solver);
  return Res;
}

SMTCondition SMTCondition::operator!() const {
  if (isTrue()) {
    return getFalseCond();
  }

  if (isFalse()) {
    return getTrueCond();
  }

  auto ResConstr = !*Constr;
  auto Res = SMTCondition(ResConstr, Solver);
  return Res;
}

bool SMTCondition::operator==(const SMTCondition &R) const {
  return Index == R.Index;
}

bool SMTCondition::operator!=(const SMTCondition &R) const {
  return !(*this == R);
}

bool SMTCondition::operator<(const SMTCondition &R) const {
  return Index < R.Index;
}

size_t SMTCondition::hash() const { return std::hash<int64_t>()(Index); }

int64_t SMTCondition::getIndex() const { return Index; }

static SMTCondition getUnknownConstraint(PathCondSolver *SolverCtx) {
  static std::atomic<size_t> UnknownCnt(0);
  return SMTCondition(
      SolverCtx->buildBoolUnknown("Un_" + std::to_string(UnknownCnt++)),
      SolverCtx);
}

SMTCondition SMTCondition::translate(PathCondSolver *NewSolver,
                                     bool &ToUnknown) const {
  assert(NewSolver);
  assert(Solver != NewSolver);
  if (isLit()) {
    return *this;
  }

  std::lock_guard<std::mutex> LK(Solver->getSolverLock());
  ToUnknown = false;

  if (isConstrComplex()) {
    ToUnknown = true;
    return getUnknownConstraint(NewSolver);
  }

  auto NewCons = NewSolver->translate(*Constr);
  return SMTCondition(NewCons, NewSolver);
}

SMTCondition SMTCondition::translateNoLock(PathCondSolver *NewSolver) const {
  assert(NewSolver);
  assert(Solver != NewSolver);
  if (isLit()) {
    return *this;
  }

  if (isConstrComplex()) {
    return getUnknownConstraint(NewSolver);
  }

  auto NewCons = NewSolver->translate(*Constr);
  return SMTCondition(NewCons, NewSolver);
}

SMTCondition SMTCondition::rename(const std::string &Suffix) const {
  if (isLit()) {
    return *this;
  }

  auto RenamedExpr = Solver->renameExpr(*Constr, Suffix);
  return SMTCondition(RenamedExpr, Solver);
}

bool SMTCondition::isConstrComplex() const {
  if (isLit()) {
    return false;
  }

  auto LimitVal = AnalysisLimit::CONSTRAINT_SIZE_LIMIT_V;
  return Solver->getExprSize(*Constr, LimitVal) >= LimitVal;
}

SMTExprVec SMTCondition::toSMT(PathCondSolver &S) const {
  if (isLit()) {
    auto Vec = S.createEmptySMTExprVec();
    if (Lit == 1) {
      Vec.push_back(S.buildBoolVal(true));
    } else {
      Vec.push_back(S.buildBoolVal(false));
    }
    return Vec;
  } else {
    assert(&S == Solver);
    auto Vec = Solver->createEmptySMTExprVec();
    Vec.push_back(*Constr);
    return Vec;
  }
}
