#include "SymbolicExecution/Domain/SymbolicMemory.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"

using namespace SymbolicExecution;

namespace std {
size_t hash<SymbolicExecution::AccessPath>::operator()(
    const SymbolicExecution::AccessPath &V) const {
  return V.hash();
}

size_t hash<SymbolicExecution::PTItem>::operator()(
    const SymbolicExecution::PTItem &V) const {
  return V.hash();
}
} // namespace std

PTItem::PTItem(ProgramValuePtr AllocSite, MemObjKind K, int64_t Off,
               const PropertyValuePtr &Sz)
    : AP(std::move(AllocSite), GetProperty<PropertyInteger>(Off)), Size(Sz),
      K(K) {}

bool PTItem::isSymbolic() const { return K == MK_SYMBOLIC; }

bool PTItem::isConcrete() const { return K == MK_CONCRETE; }

bool PTItem::isPlaceHolder() const { return K == MK_PLACEHOLDER; }

bool PTItem::isOffsetSymbolic() const {
  return IsaProperty<PropertySymExpr>(getOffset());
}

BigInteger PTItem::getConstOffset() const {
  return CastProperty<PropertyInteger>(getOffset())->getVal();
}

PTItem PTItem::offsetBy(const PropertyValuePtr &Off) const {
  PTItem Res(*this);
  Res.AP.Offset = Res.AP.Offset + Off;
  return Res;
}

PTItem PTItem::offsetBy(int64_t Off) const {
  PTItem Res(*this);
  Res.AP.Offset = Res.AP.Offset + PropertyInteger(Off);
  return Res;
}

void PTItem::changeOffsetBy(const PropertyValuePtr &Off) {
  AP.Offset = AP.Offset + Off;
}

std::string PTItem::getID() const { return AP.getID(); }
