#pragma once

#include "Alias/InclusionBased/TPA/Util/Hashing.h"

#include <cassert>
#include <cstddef>

namespace context {
class Context;
} // namespace context

namespace llvm {
class Value;
} // namespace llvm

namespace tpa {

// Pointer class for SSA-style pointer representation
//
// A Pointer represents a pointer variable in a specific calling context.
// This is the fundamental unit of context-sensitive pointer analysis.
//
// Pointer Identity:
// - A Pointer is identified by the pair (context, LLVM value)
// - The same SSA variable in different contexts is a different Pointer
// - This allows distinguishing flow through different call paths
//
// Example:
//   void foo(int* p) { bar(p); }
//   void baz(int* p) { qux(p); }
//
//   In bar(), p has Pointer(global_context, p_arg)
//   In baz(), p has Pointer(global_context, p_arg)
//   But in qux() called from bar(), p has Pointer([bar->qux], p_arg)
//   And in qux() called from baz(), p has Pointer([baz->qux], p_arg)
//
// These are four different Pointers, enabling context-sensitive analysis
class Pointer {
private:
  const context::Context *ctx;
  const llvm::Value *value;

  // Conversion to pair for hashing and storage
  using PairType = std::pair<const context::Context *, const llvm::Value *>;

  // Private constructor - created by PointerManager only
  Pointer(const context::Context *c, const llvm::Value *v) : ctx(c), value(v) {
    assert(c != nullptr && v != nullptr);
  }

public:
  // Get the calling context of this pointer
  const context::Context *getContext() const { return ctx; }
  // Get the LLVM value this pointer represents
  const llvm::Value *getValue() const { return value; }

  // Equality: same context and same value
  bool operator==(const Pointer &rhs) const {
    return ctx == rhs.ctx && value == rhs.value;
  }
  bool operator!=(const Pointer &rhs) const { return !(*this == rhs); }

  // Allow conversion to pair for use in containers
  operator PairType() const { return std::make_pair(ctx, value); }

  friend class PointerManager;
};

} // namespace tpa

namespace std {

template <> struct hash<tpa::Pointer> {
  size_t operator()(const tpa::Pointer &ptr) const {
    return util::hashPair(ptr.getContext(), ptr.getValue());
  }
};

} // namespace std
