/** @file PropertyAllocator.h @brief Allocation and casting helpers for PropertyValue handles. */
#pragma once

#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"
#include "SymbolicExecution/Domain/PropertyValue.h"

#include "llvm/Support/Casting.h"

#include <cassert>
#include <memory>
#include <utility>

namespace SymbolicExecution {

template <typename Ty, typename... ArgTys>
std::shared_ptr<Ty> GetProperty(ArgTys &&...Args) {
  return std::make_shared<Ty>(std::forward<ArgTys>(Args)...);
}

template <typename Ty> bool IsaProperty(const PropertyValuePtr &Val) {
  const PropertyValue *VPtr = Val.get();
  return VPtr != nullptr && llvm::isa<Ty>(VPtr);
}

template <typename Ty> const Ty *CastProperty(const PropertyValuePtr &Val) {
  assert(IsaProperty<Ty>(Val));
  const PropertyValue *VPtr = Val.get();
  return llvm::cast<Ty>(VPtr);
}

} // namespace SymbolicExecution
