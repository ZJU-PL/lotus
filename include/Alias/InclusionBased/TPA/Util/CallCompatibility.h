#pragma once

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace tpa {
/// Reject incompatible value representations while allowing typed-pointer
/// casts used by C callbacks. This is a candidate filter, not an ABI proof.
inline bool compatibleCallType(const llvm::Type *actual,
                               const llvm::Type *formal) {
  if (actual == formal)
    return true;
  if (actual->isPointerTy() && formal->isPointerTy())
    return actual->getPointerAddressSpace() == formal->getPointerAddressSpace();
  const auto *av = llvm::dyn_cast<llvm::VectorType>(actual);
  const auto *fv = llvm::dyn_cast<llvm::VectorType>(formal);
  if (av && fv)
    return av->getElementCount() == fv->getElementCount() &&
           compatibleCallType(av->getElementType(), fv->getElementType());
  const auto *aa = llvm::dyn_cast<llvm::ArrayType>(actual);
  const auto *fa = llvm::dyn_cast<llvm::ArrayType>(formal);
  if (aa && fa)
    return aa->getNumElements() == fa->getNumElements() &&
           compatibleCallType(aa->getElementType(), fa->getElementType());
  const auto *as = llvm::dyn_cast<llvm::StructType>(actual);
  const auto *fs = llvm::dyn_cast<llvm::StructType>(formal);
  if (as && fs && !as->isOpaque() && !fs->isOpaque() &&
      as->isPacked() == fs->isPacked() &&
      as->getNumElements() == fs->getNumElements()) {
    for (unsigned i = 0; i < as->getNumElements(); ++i)
      if (!compatibleCallType(as->getElementType(i), fs->getElementType(i)))
        return false;
    return true;
  }
  // Integer widths, floating formats, void and non-pointer categories differ.
  return false;
}

inline bool compatibleCallTarget(const llvm::CallBase &call,
                                 const llvm::Function &target) {
  const auto *type = target.getFunctionType();
  if (call.getCallingConv() != target.getCallingConv() ||
      !compatibleCallType(call.getType(), type->getReturnType()) ||
      call.arg_size() < type->getNumParams() ||
      (!type->isVarArg() && call.arg_size() != type->getNumParams()))
    return false;
  for (unsigned i = 0; i < type->getNumParams(); ++i)
    if (!compatibleCallType(call.getArgOperand(i)->getType(),
                            type->getParamType(i)))
      return false;
  return true;
}
} // namespace tpa
