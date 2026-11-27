#include "SymbolicExecution/Domain/PropertySym.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertyValue.h"
#include "SymbolicExecution/Support/HashUtil.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace SymbolicExecution;

PropertySymExpr::PropertySymExpr(Var V) : PropertyValue(VK_SymExpr) {
  addTerm(V, 1);
}

PropertySymExpr::PropertySymExpr(Var V, BigInteger C)
    : PropertyValue(VK_SymExpr) {
  addTerm(V, C);
}

PropertySymExpr::PropertySymExpr() : PropertyValue(VK_SymExpr) {}

void PropertySymExpr::addTerm(Var V, BigInteger C) {
  if (V.getValue().isVacuous()) {
    llvm::errs() << "Warning: addTerm called with vacuous Var, skipping\n";
    return;
  }

  if (V.isConstant()) {
    offsets += C * V.getAsConstant();
    return;
  }

  coeffs[V] += C;
  if (coeffs[V] == 0) {
    coeffs.erase(V);
  }
}

void PropertySymExpr::setConstTerm(const BigInteger &C) { offsets = C; }

void PropertySymExpr::addConstTerm(const BigInteger &C) { offsets += C; }

void PropertySymExpr::dumpDbgString(raw_ostream &O) const {
  if (isConstant()) {
    APInt Offset = offsets.getVal();
    O << "sym:" << llvm::toString(Offset, 10, true) << "\n";
    return;
  }

  std::string VPrefix = "X";
  unsigned Idx = 0;
  std::string Rep;
  std::map<unsigned, ProgramValuePtr> IdxToValue;

  for (const auto &P : coeffs) {
    std::string CoeffStr;
    if (P.second != 1) {
      CoeffStr = llvm::toString(P.second.getVal(), 10, true);
    }

    Rep += CoeffStr + VPrefix + std::to_string(Idx);
    if (Idx != coeffs.size() - 1) {
      Rep += "+";
    }

    IdxToValue.insert(std::make_pair(Idx, P.first.getValue()));
    ++Idx;
  }

  if (offsets != 0) {
    Rep += "+";
    Rep += llvm::toString(offsets.getVal(), 10, true);
  }

  O << "sym:" + Rep << "\n";
  for (const auto &P : IdxToValue) {
    std::string ValS = VPrefix + std::to_string(P.first) + "=";
    O << ValS << P.second.getID() << "\n";
  }
}

Function *PropertySymExpr::getEnclosingFunc() const {
  if (isConstant()) {
    return nullptr;
  }

  Function *Func = nullptr;
  for (const auto &Entry : coeffs) {
    Function *CurFunc = Entry.first.getEnclosingFunc();
    if (CurFunc) {
      if (!Func) {
        Func = CurFunc;
      } else if (Func != CurFunc) {
        llvm::errs() << "[Bug] symexpr with vars from different functions.\n";
        return nullptr;
      }
    }
  }

  return Func;
}

PropertySymExpr PropertySymExpr::operator+(const PropertySymExpr &R) const {
  PropertySymExpr ResExpr(*this);
  for (const auto &Entry : R.coeffs) {
    ResExpr.addTerm(Entry.first, Entry.second);
  }

  ResExpr.offsets += R.offsets;
  return ResExpr;
}

PropertySymExpr PropertySymExpr::operator-(const PropertySymExpr &R) const {
  PropertySymExpr ResExpr(*this);
  for (const auto &Entry : R.coeffs) {
    ResExpr.addTerm(Entry.first, -Entry.second);
  }

  ResExpr.offsets -= R.offsets;
  return ResExpr;
}

PropertySymExpr PropertySymExpr::operator*(const BigInteger &R) const {
  if (R == 0) {
    return PropertySymExpr();
  }
  if (R == 1) {
    return *this;
  }

  PropertySymExpr ResExpr;
  for (const auto &Entry : coeffs) {
    ResExpr.addTerm(Entry.first, Entry.second * R);
  }
  ResExpr.addConstTerm(this->offsets * R);
  return ResExpr;
}

PropertySymExpr PropertySymExpr::operator+(const BigInteger &R) const {
  PropertySymExpr ResExpr(*this);
  ResExpr.offsets += R;
  return ResExpr;
}

PropertySymExpr PropertySymExpr::operator-(const BigInteger &R) const {
  PropertySymExpr ResExpr(*this);
  ResExpr.offsets -= R;
  return ResExpr;
}

PropertySymExpr PropertySymExpr::doBinOp(const BigInteger &R, BinOp Op) const {
  if (Op == Add) {
    return this->operator+(R);
  }
  if (Op == Sub) {
    return this->operator-(R);
  }
  assert(Op == Mul);
  return this->operator*(R);
}

std::unordered_set<Var> PropertySymExpr::getUsedVars() const {
  std::unordered_set<Var> UsedVars;
  for (const auto &P : coeffs) {
    UsedVars.insert(P.first);
  }
  return UsedVars;
}

bool PropertySymExpr::isVar() const {
  return offsets == 0 && coeffs.size() == 1 && coeffs.begin()->second == 1;
}

Var PropertySymExpr::getAsVar() const {
  assert(isVar());
  return coeffs.begin()->first;
}

PropertyValuePtr
PropertySymExpr::map(const std::unordered_map<Var, PropertyValuePtr> &M) const {
  PropertySymExpr MappedExpr;
  BigInteger ExtraOffsets = 0;

  MappedExpr.offsets = offsets;
  for (const auto &Entry : coeffs) {
    Var FromV(Entry.first);
    if (M.count(FromV)) {
      auto TermVal =
          M.at(FromV)->binOp(PropertyInteger(Entry.second), PropertyValue::Mul);
      assert(TermVal);
      if (IsaProperty<PropertyInteger>(TermVal)) {
        ExtraOffsets += CastProperty<PropertyInteger>(TermVal)->getVal();
      } else {
        MappedExpr = MappedExpr + *CastProperty<PropertySymExpr>(TermVal);
      }
    } else {
      MappedExpr.addTerm(Entry.first, Entry.second);
    }
  }

  MappedExpr.offsets += ExtraOffsets;
  if (MappedExpr.isConstant()) {
    return GetProperty<PropertyInteger>(MappedExpr.getAsConstant());
  }
  return GetProperty<PropertySymExpr>(std::move(MappedExpr));
}

PropertyValuePtr PropertySymExpr::map(const std::unordered_map<Var, Var> &M,
                                      bool Total) const {
  PropertySymExpr MappedExpr;
  BigInteger ExtraOffsets = 0;

  MappedExpr.offsets = offsets;
  for (const auto &Entry : coeffs) {
    Var FromV(Entry.first);
    if (M.count(FromV)) {
      Var MappedV = M.at(FromV);
      if (MappedV.isConstant()) {
        ExtraOffsets += MappedV.getAsConstant() * Entry.second;
      } else {
        MappedExpr.addTerm(MappedV, Entry.second);
      }
    } else {
      if (Total) {
        return PropertyValuePtr();
      }
      MappedExpr.addTerm(Entry.first, Entry.second);
    }
  }

  MappedExpr.offsets += ExtraOffsets;
  if (MappedExpr.isConstant()) {
    return GetProperty<PropertyInteger>(MappedExpr.getAsConstant());
  }
  return GetProperty<PropertySymExpr>(std::move(MappedExpr));
}

PropertyValuePtr
PropertySymExpr::mapWithDefault(const std::unordered_map<Var, Var> &M,
                                Var DefaultV) const {
  auto Res = map(M);
  if (Res) {
    return Res;
  }
  if (DefaultV.isConstant()) {
    return GetProperty<PropertyInteger>(DefaultV.getAsConstant());
  }
  return GetProperty<PropertySymExpr>(DefaultV);
}

size_t PropertySymExpr::hash() const {
  size_t Seed = 0;
  auto Mix = [&Seed](size_t Val) {
    Seed ^= Val + 0x9e3779b97f4a7c15ULL + (Seed << 6) + (Seed >> 2);
  };
  for (const auto &Entry : coeffs) {
    Mix(std::hash<Var>()(Entry.first));
    Mix(llvm::hash_value(Entry.second.getVal()));
  }
  Mix(llvm::hash_value(offsets.getVal()));
  return Seed;
}
