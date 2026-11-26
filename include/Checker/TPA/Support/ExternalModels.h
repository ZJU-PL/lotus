#pragma once

#include "Annotation/ModRef/ExternalModRefTable.h"

#include <stdexcept>

#include <llvm/IR/IntrinsicInst.h>

namespace tpa::taint {
/// Ignore is deliberately optimistic: missing models add no memory or taint
/// effects. Error requires models for every resolved external call target.
enum class UnmodeledPolicy { Ignore, Error };

inline bool isDebugIntrinsic(const llvm::Function &function) {
  return function.isIntrinsic() && function.getName().startswith("llvm.dbg.");
}

inline bool isDebugIntrinsic(const llvm::Instruction &instruction) {
  return llvm::isa<llvm::DbgInfoIntrinsic>(instruction);
}

inline const annotation::ModRefEffectSummary &
lookupModRefSummary(const annotation::ExternalModRefTable &table,
                    const llvm::Function &function, UnmodeledPolicy policy) {
  static const annotation::ModRefEffectSummary noEffects;
  if (isDebugIntrinsic(function))
    return noEffects;
  if (const auto *summary = table.lookup(function.getName()))
    return *summary;
  if (policy == UnmodeledPolicy::Error)
    throw std::invalid_argument("Missing mod/ref model for " +
                                function.getName().str());
  return noEffects;
}
} // namespace tpa::taint
