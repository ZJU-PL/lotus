/** @file BigInteger.h @brief Arbitrary-precision integer support for symbolic execution. */
#pragma once

#include "llvm/ADT/APInt.h"

#include "SymbolicExecution/Domain/PropertyValue.h"

#include <cstdint>
#include <utility>

namespace SymbolicExecution {

class BigInteger {
public:
  llvm::APInt Val;

  BigInteger(int64_t V) {
    llvm::APInt Tmp(sizeof(int64_t) * 8, V, true);
    Val = llvm::APInt(Tmp.getMinSignedBits(), V, true);
  }

  BigInteger(llvm::APInt V) {
    if (V.getBitWidth() == 1) {
      Val = llvm::APInt(8, V.getZExtValue(), true);
    } else {
      Val = std::move(V);
    }
  }

  BigInteger() : BigInteger(0) {}

  BigInteger operator+(const BigInteger &R) const;
  BigInteger operator-(const BigInteger &R) const;
  BigInteger operator-() const;
  BigInteger operator*(const BigInteger &R) const;
  BigInteger &operator+=(const BigInteger &R);
  BigInteger &operator-=(const BigInteger &R);
  BigInteger &operator*=(const BigInteger &R);
  BigInteger sdiv(const BigInteger &R) const;
  BigInteger udiv(const BigInteger &R) const;

  bool operator==(const BigInteger &R) const;
  bool operator<(const BigInteger &R) const;
  bool operator<=(const BigInteger &R) const;
  bool operator>(const BigInteger &R) const;
  bool operator>=(const BigInteger &R) const;
  bool operator!=(const BigInteger &R) const;

  llvm::APInt getVal() const { return Val; }
  BigInteger doBinOp(const BigInteger &R, PropertyValue::BinOp Op) const;

  int64_t getAsBoundInt() const {
    if (Val.getMinSignedBits() > 64) {
      return 0;
    }
    return Val.getSExtValue();
  }

private:
  void normalizeWidth(llvm::APInt &L, llvm::APInt &R) const;
};

} // namespace SymbolicExecution
