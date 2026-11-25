#include "SymbolicExecution/Domain/PropertyInteger.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringExtras.h"

using namespace SymbolicExecution;

PropertyInteger::PropertyInteger(BigInteger V)
    : PropertyValue(VK_Integer), Val(std::move(V)) {}

size_t PropertyInteger::hash() const {
  return llvm::hash_value(getVal().getVal());
}

void PropertyInteger::dumpDbgString(llvm::raw_ostream &O) const {
  O << llvm::toString(Val.getVal(), 10, true) << "\n";
}
