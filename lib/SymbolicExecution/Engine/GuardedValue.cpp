//===----------------------------------------------------------------------===//
//
// Path-condition-guarded container implementations.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Engine/GuardedValue.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"

#include <numeric>

using namespace SymbolicExecution;

namespace std {
size_t hash<SymbolicExecution::Condition>::operator()(
    const SymbolicExecution::Condition &V) const {
  return V.hash();
}
} // namespace std

PtsSet PtsSet::offsetBy(const GuardedSymbolicValSet &Offs) const {
  PtsSet Res;
  forEach2(
      Offs,
      [&](const PTItem &Pt, const PropertyValuePtr &Off,
          const Condition &Cond) { Res.addValue(Pt.offsetBy(Off), Cond); },
      [&]() { return Res.isFull(); });
  return Res;
}

GuardedSymbolicValSet::GuardedSymbolicValSet(const PropertyValuePtr &V,
                                             const Condition &Cond) {
  addValue(V, Cond);
}

GuardedSymbolicValSet
GuardedSymbolicValSet::binOp(const GuardedSymbolicValSet &Rhs,
                             PropertyValue::BinOp Op, size_t Threshold) const {
  GuardedSymbolicValSet Result;
  forEach2(
      Rhs,
      [&](const PropertyValuePtr &Val1, const PropertyValuePtr &Val2,
          const Condition &Cond) {
        PropertyValuePtr ResVal(Val1->binOp(Val2, Op));
        if (ResVal) {
          Result.addValue(ResVal, Cond);
        }
      },
      [&]() { return Result.size() >= Threshold; });
  return Result;
}

GuardedSymbolicValSet GuardedSymbolicValSet::binOp(const PropertyValue &R,
                                                   PropertyValue::BinOp Op,
                                                   size_t Threshold) const {
  GuardedSymbolicValSet Result;
  forEach(
      [&](const PropertyValuePtr &Val, const Condition &Cond) {
        PropertyValuePtr ResVal(Val->binOp(R, Op));
        if (ResVal) {
          Result.addValue(ResVal, Cond);
        }
      },
      [&]() { return Result.size() >= Threshold; });
  return Result;
}

std::vector<Condition>
GuardedSymbolicValSet::cmp(const GuardedSymbolicValSet &Rhs,
                           unsigned Pred) const {
  std::vector<Condition> Res = {Condition::getFalseCond(),
                                Condition::getFalseCond(),
                                Condition::getFalseCond()};

  std::vector<std::pair<unsigned, Condition>> CmpRes =
      zip2<GuardedSymbolicValSet, unsigned>(
          Rhs, [=](const PropertyValuePtr &Val1, const PropertyValuePtr &Val2) {
            return Val1->cmp(Val2.get(), Pred);
          });

  for (const auto &P : CmpRes) {
    assert(P.first <= 2);
    Res[P.first].orCond(P.second);
  }
  return Res;
}

GuardedSymbolicValSet GuardedSymbolicValSet::seqAdd(
    std::vector<std::tuple<GuardedSymbolicValSet::IterType,
                           GuardedSymbolicValSet::IterType, BigInteger,
                           ProgramValuePtr>>
        Seq,
    PathCondSolver *Solver, bool AddEqCond, const std::string &CSSuffix) {
  GuardedSymbolicValSet Res;
  std::vector<std::pair<PropertyValuePtr, Condition>> Selections(Seq.size());

  bool Advanced = false;
  do {
    Advanced = false;
    for (size_t I = 0; I < Seq.size(); ++I) {
      IterType &CurIter = std::get<0>(Seq[I]);
      IterType &EndIter = std::get<1>(Seq[I]);
      const BigInteger &Coeff = std::get<2>(Seq[I]);
      ProgramValuePtr Variable = std::get<3>(Seq[I]);

      if (CurIter == EndIter) {
        continue;
      }

      auto Val = CurIter->first * PropertyInteger(Coeff);
      Condition Cond = CurIter->second;
      if (AddEqCond) {
        Condition EqCond(Solver->buildEqualCond(Var(Variable),
                                                CurIter->first.get(), CSSuffix),
                         Solver);
        Cond = Cond && EqCond;
      }
      Selections[I] = std::make_pair(Val, CurIter->second && Cond);
      ++CurIter;
      Advanced = true;
    }

    if (Advanced) {
      auto CurrentRun = std::accumulate(
          std::next(Selections.begin()), Selections.end(), Selections[0],
          [](const std::pair<PropertyValuePtr, Condition> &L,
             const std::pair<PropertyValuePtr, Condition> &R) {
            auto Val = L.first + R.first;
            assert(Val);
            return std::make_pair(Val, L.second && R.second);
          });
      Res.addValue(CurrentRun.first, CurrentRun.second);
    }
  } while (Advanced && !Res.isFull());

  return Res;
}
