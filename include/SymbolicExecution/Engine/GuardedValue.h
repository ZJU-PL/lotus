/** @file GuardedValue.h @brief Path-condition-guarded containers for symbolic state. */
#pragma once

#include "SymbolicExecution/Domain/ProgramVar.h"
#include "SymbolicExecution/Domain/PropertyValue.h"
#include "SymbolicExecution/Domain/SymbolicMemory.h"
#include "SymbolicExecution/Solver/ConstraintRepr.h"
#include "SymbolicExecution/Support/AnalysisLimit.h"

#include <cassert>
#include <functional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SymbolicExecution {

class GuardedSymbolicValSet;

/// CRTP container for sets of facts guarded by path conditions.
template <typename Derived, typename KeyTy,
          unsigned *Threshold = &AnalysisLimit::VALUE_SET_LIMIT_V>
class GuardedSet {
public:
  GuardedSet() = default;

  template <typename DestTy> DestTy convertTo() {
    DestTy Res;
    for (const auto &P : Vals) {
      if (Res.isFull()) {
        break;
      }
      Res.setKeyCond(P.first, P.second);
    }
    return Res;
  }

  void setKeyCond(const KeyTy &K, const Condition &Cond) { Vals[K] = Cond; }

  Derived merge(const Derived &R) const {
    Derived Res(static_cast<const Derived &>(*this));
    Res.addValues(R);
    return Res;
  }

  void translate(PathCondSolver *NewSolver) {
    for (auto &P : Vals) {
      P.second = P.second.translateNoLock(NewSolver);
    }
  }

  bool isFull() const { return Vals.size() >= *Threshold; }
  static unsigned getThreshold() { return *Threshold; }

  bool addValue(const KeyTy &Val, const Condition &Cond = Condition()) {
    if (Cond.isFalse() || isFull()) {
      return false;
    }

    if (Vals.count(Val)) {
      Condition OldCond = Vals.at(Val);
      Condition NewCond = OldCond || Cond;
      if (NewCond != OldCond) {
        setKeyCond(Val, NewCond);
      }
      return false;
    }
    setKeyCond(Val, Cond);
    return true;
  }

  template <typename OtherTy>
  void addValues(const OtherTy &R, const Condition &Cond = Condition()) {
    if (Cond.isFalse() || isFull()) {
      return;
    }

    for (const auto &ValCond : R) {
      addValue(ValCond.first, ValCond.second && Cond);
    }
  }

  void setValues(Derived R) {
    static_cast<Derived &>(*this) = std::move(R);
  }

  bool hasValue(const KeyTy &Val) const { return Vals.count(Val) != 0; }
  Condition getGuardForValue(const KeyTy &Val) const { return Vals.at(Val); }

  Derived operator&&(const Condition &Cond) const {
    Derived Res;
    for (const auto &P : Vals) {
      Condition C = P.second && Cond;
      if (!C.isFalse()) {
        Res.setKeyCond(P.first, C);
      }
    }
    return Res;
  }

  bool empty() const { return Vals.empty(); }
  size_t size() const { return Vals.size(); }

  typename std::unordered_map<KeyTy, Condition>::iterator begin() {
    return Vals.begin();
  }
  typename std::unordered_map<KeyTy, Condition>::iterator end() {
    return Vals.end();
  }
  typename std::unordered_map<KeyTy, Condition>::const_iterator begin() const {
    return Vals.begin();
  }
  typename std::unordered_map<KeyTy, Condition>::const_iterator end() const {
    return Vals.end();
  }

  void clear() { Vals.clear(); }
  bool isSingleton() const { return Vals.size() == 1; }

  const KeyTy &getAsSingleton() const {
    assert(isSingleton());
    return Vals.begin()->first;
  }

  const std::unordered_map<KeyTy, Condition> &getVals() const { return Vals; }

  using IterType =
      typename std::unordered_map<KeyTy, Condition>::const_iterator;
  using ElemType = KeyTy;

  void forEach(
      std::function<void(const KeyTy &K, const Condition &Cond)> Proc,
      std::function<bool()> StopCond = []() { return false; }) const {
    for (const auto &P : *this) {
      if (StopCond()) {
        return;
      }
      Proc(P.first, P.second);
    }
  }

  template <typename OtherTy>
  void forEach2(
      const OtherTy &Other,
      std::function<void(const KeyTy &K1, const typename OtherTy::ElemType &K2,
                         const Condition &C)>
          Proc,
      std::function<bool()> StopCond = []() { return false; }) const {
    for (const auto &P1 : *this) {
      for (const auto &P2 : Other) {
        Proc(P1.first, P2.first, P1.second && P2.second);
        if (StopCond()) {
          return;
        }
      }
    }
  }

  template <typename OtherTy, typename ResTy>
  std::vector<std::pair<ResTy, Condition>>
  zip2(const OtherTy &Other,
       std::function<ResTy(const KeyTy &K1,
                           const typename OtherTy::ElemType &K2)>
           Proc) const {
    std::vector<std::pair<ResTy, Condition>> Result;
    for (const auto &P1 : *this) {
      for (const auto &P2 : Other) {
        Result.emplace_back(Proc(P1.first, P2.first), P1.second && P2.second);
      }
    }
    return Result;
  }

  template <typename OtherTy>
  OtherTy
  map(std::function<typename OtherTy::ElemType(const KeyTy &K1)> Proc) const {
    OtherTy Res;
    for (const auto &P : *this) {
      Res.addValue(Proc(P.first), P.second);
    }
    return Res;
  }

  template <typename ResTy>
  ResTy reduce(
      std::function<void(const KeyTy &, const Condition &, ResTy &)> Proc,
      std::function<bool(const ResTy &)> StopCond = [](const ResTy &) {
        return false;
      }) const {
    ResTy Res;
    for (const auto &P : *this) {
      Proc(P.first, P.second, Res);
      if (StopCond(Res)) {
        return Res;
      }
    }
    return Res;
  }

protected:
  std::unordered_map<KeyTy, Condition> Vals;
};

class PtsSet
    : public GuardedSet<PtsSet, PTItem, &AnalysisLimit::POINTS_SET_LIMIT_V> {
public:
  PtsSet offsetBy(const GuardedSymbolicValSet &Offs) const;
};

/// Symbolic values paired with the conditions under which each value holds.
class GuardedSymbolicValSet
    : public GuardedSet<GuardedSymbolicValSet, PropertyValuePtr,
                        &AnalysisLimit::SYMBOLIC_VAL_SET_LIMIT_V> {
public:
  static GuardedSymbolicValSet
  seqAdd(std::vector<std::tuple<GuardedSymbolicValSet::IterType,
                                GuardedSymbolicValSet::IterType, BigInteger,
                                ProgramValuePtr>>
             Seq,
         PathCondSolver *Solver, bool AddEqCond, const std::string &CSSuffix);

  GuardedSymbolicValSet() = default;
  GuardedSymbolicValSet(const PropertyValuePtr &V,
                        const Condition &Cond = Condition());

  GuardedSymbolicValSet
  binOp(const GuardedSymbolicValSet &Rhs, PropertyValue::BinOp Op,
        size_t Threshold = AnalysisLimit::SYMBOLIC_VAL_SET_LIMIT_V) const;

  GuardedSymbolicValSet
  binOp(const PropertyValue &R, PropertyValue::BinOp Op,
        size_t Threshold = AnalysisLimit::SYMBOLIC_VAL_SET_LIMIT_V) const;

  GuardedSymbolicValSet operator+(const GuardedSymbolicValSet &Rhs) const {
    return binOp(Rhs, PropertyValue::Add);
  }
  GuardedSymbolicValSet operator-(const GuardedSymbolicValSet &Rhs) const {
    return binOp(Rhs, PropertyValue::Sub);
  }
  GuardedSymbolicValSet operator*(const GuardedSymbolicValSet &Rhs) const {
    return binOp(Rhs, PropertyValue::Mul);
  }
  GuardedSymbolicValSet operator+(const PropertyValue &Rhs) const {
    return binOp(Rhs, PropertyValue::Add);
  }
  GuardedSymbolicValSet operator-(const PropertyValue &Rhs) const {
    return binOp(Rhs, PropertyValue::Sub);
  }
  GuardedSymbolicValSet operator*(const PropertyValue &Rhs) const {
    return binOp(Rhs, PropertyValue::Mul);
  }

  std::vector<Condition> cmp(const GuardedSymbolicValSet &Rhs,
                             unsigned Pred) const;
};

class GuardedProgramValSet
    : public GuardedSet<GuardedProgramValSet, ProgramValuePtr> {
public:
  GuardedProgramValSet() = default;
  GuardedProgramValSet(const ProgramValuePtr &V) { addValue(V); }
};

class GuardedAccessPathSet
    : public GuardedSet<GuardedAccessPathSet, AccessPath> {};

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::Condition> {
  size_t operator()(const SymbolicExecution::Condition &V) const;
};
} // namespace std
