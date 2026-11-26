/** @file PropertyInteger.h @brief Integer-typed abstract property value for symbolic execution. */
#pragma once

#include "SymbolicExecution/Domain/BigInteger.h"
#include "SymbolicExecution/Domain/PropertyValue.h"

namespace SymbolicExecution {

class PropertyInteger : public PropertyValue {
public:
  explicit PropertyInteger(BigInteger V);

  static bool classof(const PropertyValue *V) {
    return V->getKind() == VK_Integer;
  }

  BigInteger getVal() const { return Val; }
  size_t hash() const override;
  bool operator==(const PropertyInteger &R) const { return Val == R.Val; }
  int64_t getAsBoundInt() const { return Val.getAsBoundInt(); }
  void dumpDbgString(llvm::raw_ostream &O) const override;
  PropertyValue *clone() const override { return new PropertyInteger(*this); }

private:
  BigInteger Val;
};

} // namespace SymbolicExecution
