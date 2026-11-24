#pragma once

#include "llvm/IR/Instructions.h"
#include "llvm/ADT/Optional.h"
#include "llvm/IR/ConstantRange.h"

#include <memory>
#include <utility>

namespace pdg {

/// Reusable LLVM analyses for rules. Results are valid only while the function
/// is unchanged. Load equivalence requires an unmodified memory location.
class FunctionFacts {
public:
  explicit FunctionFacts(llvm::Function &function);
  ~FunctionFacts();
  bool equivalent(const llvm::Value &left, const llvm::Value &right) const;
  /// Resolve a pointer loaded from must-alias reaching stores. Ambiguous or
  /// clobbered loads retain their own identity, rather than guessing an alias.
  const llvm::Value &pointerOrigin(const llvm::Value &value) const;
  bool reaches(const llvm::Instruction &from,
               const llvm::Instruction &to) const;
  bool blocksConnected(const llvm::BasicBlock &left,
                       const llvm::BasicBlock &right) const;
  bool recursive(const llvm::CallBase &call) const;
  bool repeatedStackAllocation(const llvm::AllocaInst &allocation) const;
  bool onlyZeroChecked(const llvm::CallBase &call) const;
  bool nonNegative(const llvm::Value &value, const llvm::Instruction &at) const;
  bool mayOverflowPositively(const llvm::BinaryOperator &add) const;
  /// A finite unsigned bound established by the controlling CFG edge or by
  /// range-reducing operations. Signed guards also need a nonnegative proof.
  bool boundedAt(const llvm::Value &value, const llvm::Instruction &at) const;
  /// Conservative machine-integer range, refined by dominating comparisons.
  llvm::Optional<llvm::ConstantRange>
  integerRangeAt(const llvm::Value &value, const llvm::Instruction &at) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pdg
