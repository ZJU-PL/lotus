#include "SymbolicExecution/Domain/BigInteger.h"

#include <cassert>
#include <functional>

using namespace llvm;
using namespace SymbolicExecution;

BigInteger BigInteger::operator+(const BigInteger &R) const {
  return doBinOp(R, PropertyValue::Add);
}

BigInteger BigInteger::operator-(const BigInteger &R) const {
  return doBinOp(R, PropertyValue::Sub);
}

BigInteger BigInteger::operator-() const {
  return BigInteger(APInt(getVal().getBitWidth(), 0, true)) - (*this);
}

BigInteger BigInteger::operator*(const BigInteger &R) const {
  return doBinOp(R, PropertyValue::Mul);
}

BigInteger &BigInteger::operator+=(const BigInteger &R) {
  (*this) = this->operator+(R);
  return *this;
}

BigInteger &BigInteger::operator*=(const BigInteger &R) {
  (*this) = this->operator*(R);
  return *this;
}

BigInteger &BigInteger::operator-=(const BigInteger &R) {
  (*this) = this->operator-(R);
  return *this;
}

BigInteger BigInteger::sdiv(const BigInteger &R) const {
  return doBinOp(R, PropertyValue::SDiv);
}

BigInteger BigInteger::udiv(const BigInteger &R) const {
  return doBinOp(R, PropertyValue::UDiv);
}

bool BigInteger::operator==(const BigInteger &R) const {
  APInt LVal = getVal();
  APInt RVal = R.getVal();
  normalizeWidth(LVal, RVal);
  return LVal == RVal;
}

bool BigInteger::operator<(const BigInteger &R) const {
  APInt LVal = getVal();
  APInt RVal = R.getVal();
  normalizeWidth(LVal, RVal);
  return LVal.slt(RVal);
}

bool BigInteger::operator<=(const BigInteger &R) const {
  APInt LVal = getVal();
  APInt RVal = R.getVal();
  normalizeWidth(LVal, RVal);
  return LVal.sle(RVal);
}

bool BigInteger::operator>(const BigInteger &R) const {
  APInt LVal = getVal();
  APInt RVal = R.getVal();
  normalizeWidth(LVal, RVal);
  return LVal.sgt(RVal);
}

bool BigInteger::operator>=(const BigInteger &R) const {
  APInt LVal = getVal();
  APInt RVal = R.getVal();
  normalizeWidth(LVal, RVal);
  return LVal.sge(RVal);
}

bool BigInteger::operator!=(const BigInteger &R) const {
  return !((*this) == R);
}

BigInteger BigInteger::doBinOp(const BigInteger &R,
                               PropertyValue::BinOp Op) const {
  APInt ResVal = Val;
  APInt RightVal = R.Val;

  normalizeWidth(ResVal, RightVal);

  bool Overflow = false;

  if (Op == PropertyValue::UDiv) {
    return BigInteger(ResVal.udiv(RightVal));
  }

  std::function<APInt(const APInt &, const APInt &, bool &)> Func;
  if (Op == PropertyValue::Add) {
    Func = &APInt::sadd_ov;
  } else if (Op == PropertyValue::Sub) {
    Func = &APInt::ssub_ov;
  } else if (Op == PropertyValue::Mul) {
    Func = &APInt::smul_ov;
  } else if (Op == PropertyValue::SDiv) {
    Func = &APInt::sdiv_ov;
  } else {
    assert(false);
  }

  ResVal = Func(ResVal, RightVal, Overflow);
  if (Overflow) {
    ResVal = Val;
    RightVal = R.Val;

    normalizeWidth(ResVal, RightVal);

    ResVal = ResVal.sext(ResVal.getBitWidth() * 2);
    RightVal = RightVal.sext(RightVal.getBitWidth() * 2);
    ResVal = Func(ResVal, RightVal, Overflow);
  }

  return BigInteger(std::move(ResVal));
}

void BigInteger::normalizeWidth(APInt &L, APInt &R) const {
  if (L.getBitWidth() > R.getBitWidth()) {
    R = R.sext(L.getBitWidth());
  } else if (L.getBitWidth() < R.getBitWidth()) {
    L = L.sext(R.getBitWidth());
  }

  assert(R.getBitWidth() == L.getBitWidth());
}
