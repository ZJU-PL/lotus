/** @file PropertySym.h @brief Affine symbolic property expression. */
#pragma once

#include "SymbolicExecution/Domain/BigInteger.h"
#include "SymbolicExecution/Domain/ProgramVar.h"
#include "SymbolicExecution/Domain/PropertyValue.h"

#include <cassert>
#include <map>
#include <unordered_set>

namespace SymbolicExecution {

/// Affine symbolic expression: a1*x1 + ... + an*xn + c.
class PropertySymExpr : public PropertyValue {
public:
  explicit PropertySymExpr(Var V);
  PropertySymExpr(Var V, BigInteger C);
  PropertySymExpr();

  static bool classof(const PropertyValue *V) {
    return V->getKind() == VK_SymExpr;
  }

  PropertySymExpr operator+(const PropertySymExpr &R) const;
  PropertySymExpr operator-(const PropertySymExpr &R) const;
  PropertySymExpr operator*(const BigInteger &R) const;
  PropertySymExpr operator+(const BigInteger &R) const;
  PropertySymExpr operator-(const BigInteger &R) const;
  PropertySymExpr doBinOp(const BigInteger &R, BinOp Op) const;

  bool isConstant() const { return coeffs.empty(); }

  BigInteger getAsConstant() const {
    assert(isConstant());
    return offsets;
  }

  bool isVar() const;
  Var getAsVar() const;

  PropertyValuePtr map(const std::unordered_map<Var, Var> &M,
                       bool Total = true) const override;
  PropertyValuePtr
  map(const std::unordered_map<Var, PropertyValuePtr> &M) const override;
  PropertyValuePtr mapWithDefault(const std::unordered_map<Var, Var> &M,
                                  Var DefaultV) const override;

  std::map<Var, BigInteger>::iterator begin() { return coeffs.begin(); }
  std::map<Var, BigInteger>::iterator end() { return coeffs.end(); }
  std::map<Var, BigInteger>::const_iterator begin() const {
    return coeffs.begin();
  }
  std::map<Var, BigInteger>::const_iterator end() const { return coeffs.end(); }

  BigInteger getOffsets() const { return offsets; }
  size_t hash() const override;
  std::unordered_set<Var> getUsedVars() const;

  bool operator==(const PropertySymExpr &R) const {
    return coeffs == R.coeffs && offsets == R.offsets;
  }

  void addTerm(Var var, BigInteger C);
  void setConstTerm(const BigInteger &C);
  void addConstTerm(const BigInteger &C);
  void dumpDbgString(llvm::raw_ostream &O) const override;

  const std::map<Var, BigInteger> &getCoeffs() const { return coeffs; }
  llvm::Function *getEnclosingFunc() const override;
  PropertyValue *clone() const override { return new PropertySymExpr(*this); }

private:
  std::map<Var, BigInteger> coeffs;
  BigInteger offsets = 0;
};

} // namespace SymbolicExecution
