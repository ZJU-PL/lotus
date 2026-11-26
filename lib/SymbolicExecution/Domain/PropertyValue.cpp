#include "SymbolicExecution/Domain/PropertyValue.h"

#include "SymbolicExecution/Domain/ProgramVar.h"
#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"

#include "llvm/Support/raw_ostream.h"

#include <cassert>

using namespace llvm;
using namespace SymbolicExecution;

PropertyValue::PropertyValue(PropertyValueKind ValK) : ValKind(ValK) {}

PropertyValue::BinOp PropertyValue::fromLLVMOp(unsigned OpCode) {
  if (OpCode == Instruction::Add) {
    return Add;
  }
  if (OpCode == Instruction::Sub) {
    return Sub;
  }
  if (OpCode == Instruction::Mul) {
    return Mul;
  }
  if (OpCode == Instruction::SDiv) {
    return SDiv;
  }
  if (OpCode == Instruction::UDiv) {
    return UDiv;
  }
  assert(false);
  return Add;
}

unsigned PropertyValue::cmp(const PropertyValue *Rhs, unsigned Pred) const {
  bool IsConstant = false;
  APInt LeftVal, RightVal;
  if (isa<PropertyInteger>(this) && isa<PropertyInteger>(Rhs)) {
    LeftVal = cast<PropertyInteger>(this)->getVal().getVal();
    RightVal = cast<PropertyInteger>(Rhs)->getVal().getVal();
    IsConstant = true;
  } else {
    PropertyValuePtr ResVal(this->binOp(Rhs, PropertyValue::Sub));
    if (ResVal && IsaProperty<PropertyInteger>(ResVal)) {
      LeftVal = CastProperty<PropertyInteger>(ResVal)->getVal().getVal();
      RightVal = APInt(LeftVal.getBitWidth(), 0);
      IsConstant = true;
    }
  }

  int CmpRes = -1;
  if (IsConstant) {
    if (LeftVal.getBitWidth() > RightVal.getBitWidth()) {
      RightVal = RightVal.sext(LeftVal.getBitWidth());
    } else if (LeftVal.getBitWidth() < RightVal.getBitWidth()) {
      LeftVal = LeftVal.sext(RightVal.getBitWidth());
    }

    switch (Pred) {
    case CmpInst::ICMP_EQ:
      CmpRes = (LeftVal == RightVal);
      break;
    case CmpInst::ICMP_NE:
      CmpRes = (LeftVal != RightVal);
      break;
    case CmpInst::ICMP_SGE:
    case CmpInst::ICMP_UGE:
      CmpRes = (LeftVal.sge(RightVal));
      break;
    case CmpInst::ICMP_SGT:
    case CmpInst::ICMP_UGT:
      CmpRes = (LeftVal.sgt(RightVal));
      break;
    case CmpInst::ICMP_SLE:
    case CmpInst::ICMP_ULE:
      CmpRes = (LeftVal.sle(RightVal));
      break;
    case CmpInst::ICMP_SLT:
    case CmpInst::ICMP_ULT:
      CmpRes = (LeftVal.slt(RightVal));
      break;
    default:
      assert(false);
    }
  } else {
    CmpRes = 2;
  }

  assert(CmpRes == 0 || CmpRes == 1 || CmpRes == 2);
  return static_cast<unsigned>(CmpRes);
}

PropertyValuePtr PropertyValue::binOp(const PropertyValue &Rhs,
                                      BinOp Op) const {
  return binOp(&Rhs, Op);
}

PropertyValuePtr PropertyValue::binOp(const PropertyValuePtr &Rhs,
                                      BinOp Op) const {
  return binOp(Rhs.get(), Op);
}

PropertyValuePtr PropertyValue::binOp(const PropertyValue *Rhs,
                                      BinOp Op) const {
  auto LValKind = getKind();
  auto RValKind = Rhs->getKind();

  if (LValKind == VK_Integer && RValKind == VK_Integer) {
    const PropertyInteger *Left = cast<PropertyInteger>(this);
    const PropertyInteger *Right = cast<PropertyInteger>(Rhs);

    if (Right->getVal() == 0 && (Op == UDiv || Op == SDiv)) {
      return PropertyValuePtr();
    }

    return GetProperty<PropertyInteger>(
        Left->getVal().doBinOp(Right->getVal(), Op));
  }

  if (LValKind == VK_SymExpr && RValKind == VK_SymExpr) {
    const PropertySymExpr *Left = cast<PropertySymExpr>(this);
    const PropertySymExpr *Right = cast<PropertySymExpr>(Rhs);
    assert(!Left->isConstant());
    assert(!Right->isConstant());

    PropertySymExpr Res;
    if (Op == PropertyValue::Add) {
      Res = *Left + *Right;
    } else if (Op == PropertyValue::Sub) {
      Res = *Left - *Right;
    } else {
      return PropertyValuePtr();
    }

    if (Res.isConstant()) {
      return GetProperty<PropertyInteger>(Res.getAsConstant());
    }
    return GetProperty<PropertySymExpr>(std::move(Res));
  }

  if (LValKind == VK_Integer && RValKind == VK_SymExpr) {
    const PropertyInteger *Left = cast<PropertyInteger>(this);
    const PropertySymExpr *Right = cast<PropertySymExpr>(Rhs);
    assert(!Right->isConstant());

    PropertySymExpr Res;
    if (Op == PropertyValue::Add) {
      Res = *Right + Left->getVal();
    } else if (Op == PropertyValue::Sub) {
      Res = *Right * (-1) + Left->getVal();
    } else if (Op == PropertyValue::Mul) {
      Res = *Right * Left->getVal();
    } else {
      return PropertyValuePtr();
    }

    if (Res.isConstant()) {
      return GetProperty<PropertyInteger>(Res.getAsConstant());
    }
    return GetProperty<PropertySymExpr>(std::move(Res));
  }

  if (LValKind == VK_SymExpr && RValKind == VK_Integer) {
    const PropertySymExpr *Left = cast<PropertySymExpr>(this);
    const PropertyInteger *Right = cast<PropertyInteger>(Rhs);
    assert(!Left->isConstant());

    PropertySymExpr Res;
    if (Op == PropertyValue::Add) {
      Res = *Left + Right->getVal();
    } else if (Op == PropertyValue::Sub) {
      Res = *Left - Right->getVal();
    } else if (Op == PropertyValue::Mul) {
      Res = *Left * Right->getVal();
    } else {
      return PropertyValuePtr();
    }

    if (Res.isConstant()) {
      return GetProperty<PropertyInteger>(Res.getAsConstant());
    }
    return GetProperty<PropertySymExpr>(std::move(Res));
  }

  assert(false);
  return PropertyValuePtr();
}

PropertyValuePtr PropertyValue::map(const std::unordered_map<Var, Var> &,
                                    bool) const {
  return PropertyValuePtr(std::shared_ptr<PropertyValue>(clone()));
}

PropertyValuePtr PropertyValue::mapWithDefault(
    const std::unordered_map<Var, Var> &M, Var) const {
  return map(M);
}

PropertyValuePtr
PropertyValue::map(const std::unordered_map<Var, PropertyValuePtr> &) const {
  return PropertyValuePtr(std::shared_ptr<PropertyValue>(clone()));
}

void PropertyValue::dump() const { dumpDbgString(llvm::errs()); }

std::string PropertyValue::getName() const {
  if (ValKind == VK_Integer) {
    return std::to_string(cast<PropertyInteger>(this)->getAsBoundInt());
  }
  const auto *Val = cast<PropertySymExpr>(this);
  std::string Name = "Sym";
  if (Val->isVar()) {
    ProgramValuePtr V = Val->getAsVar().getValue();
    if (V.isa<GuardedValueFlowNodeValue>()) {
      Name = V.getLLVMVal()->getName().str();
    } else {
      Name = cast<AuxValue>(V.getData().get())->getName();
    }
  }
  return Name;
}

PropertyValuePtr::PropertyValuePtr(const std::shared_ptr<PropertyInteger> &V)
    : Data(V) {}

PropertyValuePtr::PropertyValuePtr(const std::shared_ptr<PropertySymExpr> &V)
    : Data(V) {}

PropertyValuePtr::PropertyValuePtr(const ProgramValuePtr &PV) {
  if (PV.isVacuous()) {
    Data = nullptr;
    return;
  }

  try {
    ProgramValuePtr PVCopy = PV;
    if (PVCopy.isVacuous()) {
      Data = nullptr;
      return;
    }

    Var V(std::move(PVCopy));
    if (V.getValue().isVacuous()) {
      Data = nullptr;
      return;
    }

    if (V.isConstant()) {
      Data = GetProperty<PropertyInteger>(V.getAsConstant());
    } else {
      Data = GetProperty<PropertySymExpr>(V);
    }
  } catch (...) {
    llvm::errs() << "Error: PropertyValuePtr construction failed\n";
    Data = nullptr;
  }
}

bool PropertyValuePtr::operator==(const PropertyValuePtr &R) const {
  if (Data == R.Data) {
    return true;
  }
  if (!Data || !R.Data) {
    return false;
  }
  return Data->isEquivalent(*R);
}

size_t PropertyValuePtr::hash() const {
  if (!Data) {
    return std::hash<void *>()(nullptr);
  }
  return Data->hash();
}

PropertyValuePtr PropertyValuePtr::operator-() const {
  return PropertyInteger(0).binOp(get(), PropertyValue::Sub);
}

bool PropertyValuePtr::operator<(int64_t V) const {
  return IsaProperty<PropertyInteger>(*this) &&
         CastProperty<PropertyInteger>(*this)->getVal() < V;
}

bool PropertyValuePtr::operator<=(int64_t V) const {
  return IsaProperty<PropertyInteger>(*this) &&
         CastProperty<PropertyInteger>(*this)->getVal() <= V;
}

bool PropertyValuePtr::operator==(int64_t V) const {
  return IsaProperty<PropertyInteger>(*this) &&
         CastProperty<PropertyInteger>(*this)->getVal() == V;
}

bool PropertyValuePtr::operator>(int64_t V) const {
  return IsaProperty<PropertyInteger>(*this) &&
         CastProperty<PropertyInteger>(*this)->getVal() > V;
}

bool PropertyValue::isEquivalent(const PropertyValue &Rhs) const {
  auto LK = getKind(), RK = Rhs.getKind();
  if (LK != RK) {
    return false;
  }

  if (LK == VK_Integer) {
    return *cast<PropertyInteger>(this) == *cast<PropertyInteger>(&Rhs);
  }
  assert(LK == VK_SymExpr);
  return *cast<PropertySymExpr>(this) == *cast<PropertySymExpr>(&Rhs);
}
