/** @file ConstraintRepr.h @brief Path condition representation backed by SMT expressions. */
#pragma once

#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "Solvers/SMT/LIBSMT/SMTExpr.h"
#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Support/AnalysisLimit.h"

#include "llvm/IR/Instructions.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace SymbolicExecution {

using lotus::gvfg::GuardedValueFlowRegionNode;

class PathCondSolver;

/// Solver-backed boolean condition attached to guarded values and queries.
class SMTCondition {
public:
  SMTCondition() = default;
  explicit SMTCondition(bool V);
  SMTCondition(GuardedValueFlowRegionNode *Cond, PathCondSolver *Solver);
  SMTCondition(const SMTExpr &Constr, PathCondSolver *Solver);
  SMTCondition(PropertyValuePtr L, PropertyValuePtr R,
               llvm::CmpInst::Predicate Pred, PathCondSolver *Solver);

  static SMTCondition getFalseCond() { return SMTCondition(false); }
  static SMTCondition getTrueCond() { return SMTCondition(true); }

  SMTCondition(const SMTCondition &R)
      : Index(R.Index), Lit(R.Lit), Solver(R.Solver) {
    if (R.Constr) {
      Constr = std::make_unique<SMTExpr>(*R.Constr);
    }
  }

  SMTCondition(SMTCondition &&R) noexcept
      : Index(R.Index), Lit(R.Lit), Constr(std::move(R.Constr)),
        Solver(R.Solver) {}

  SMTCondition &operator=(SMTCondition Tmp) {
    std::swap(Index, Tmp.Index);
    std::swap(Lit, Tmp.Lit);
    std::swap(Constr, Tmp.Constr);
    std::swap(Solver, Tmp.Solver);
    return *this;
  }

  bool isTrue() const;
  bool isFalse() const;

  SMTCondition operator||(const SMTCondition &R) const;
  SMTCondition operator&&(const SMTCondition &R) const;
  SMTCondition operator!() const;

  bool operator==(const SMTCondition &R) const;
  bool operator!=(const SMTCondition &R) const;
  bool operator<(const SMTCondition &R) const;
  size_t hash() const;

  void orCond(const SMTCondition &R) { *this = operator||(R); }
  void andCond(const SMTCondition &R) { *this = operator&&(R); }

  bool isLit() const { return Lit != -1; }
  int64_t getIndex() const;

  SMTCondition translate(PathCondSolver *NewSolver, bool &ToUnknown) const;
  SMTCondition translateNoLock(PathCondSolver *NewSolver) const;
  SMTCondition rename(const std::string &Suffix) const;

  SMTExpr getSMTConstr() const {
    assert(!isLit());
    return *Constr;
  }

  bool isConstrComplex() const;
  SMTExprVec toSMT(PathCondSolver &S) const;
  void dump() const;

private:
  int64_t Index = -3;
  // 1 => true; 0 => false; -1 => not a literal
  int Lit = 1;
  std::unique_ptr<SMTExpr> Constr = nullptr;
  PathCondSolver *Solver = nullptr;
};

using Condition = SMTCondition;

} // namespace SymbolicExecution
