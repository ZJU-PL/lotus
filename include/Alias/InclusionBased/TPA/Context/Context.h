#pragma once

#include "Alias/InclusionBased/TPA/Util/Hashing.h"

#include <cstddef>
#include <cstdint>
#include <unordered_set>

#include <llvm/IR/Instruction.h>

namespace context {

class ProgramPoint;

// This class represents a particualr calling context, which is represented by a
// stack of callsites
//
// A calling context represents the dynamic call stack at a particular program
// point. It enables context-sensitive analysis by distinguishing different call
// chains that lead to the same program location. This is crucial for precision
// in pointer analysis, as it allows the analysis to track different pointer
// values that may arise from different calling contexts.
//
// Design Decisions:
// - The call stack is implemented as a linked list of Context objects, where
// each
//   Context points to its predecessor (the context before the current call
//   site).
// - This representation is memory-efficient as it shares common prefixes among
//   contexts (structural sharing).
// - Contexts are interned using a global set (ctxSet) to ensure uniqueness and
//   enable efficient comparison via pointer equality.
// - The global context (empty call stack) is represented by size() == 0.
//
// Example:
//   main() -> foo() -> bar() has context [main, foo, bar]
//   main() -> baz() -> bar() has context [main, baz, bar]
//   These are different contexts even though they both end at bar()
//
// Usage:
//   - Get the global context: Context::getGlobalContext()
//   - Push a call site: Context::pushContext(ctx, inst)
//   - Pop to return: Context::popContext(ctx)
//   - Check depth: ctx->size()
//   - Check if global: ctx->isGlobalContext()
class Context {
private:
  // The call stack is implemented by a linked list
  const llvm::Instruction *callSite;
  const Context *predContext;
  size_t sz;
  mutable uint64_t id = 0;

  static std::unordered_set<Context> ctxSet;
  static const Context *insertUnlocked(const Context &candidate, bool isGlobal);

  // Private constructors - use static methods to create contexts
  // Global context constructor (empty call stack)
  Context() : callSite(nullptr), predContext(nullptr), sz(0), id(0) {}
  // Non-global context constructor (adds a call site to existing context)
  // Parameters: c - the call instruction, p - the predecessor context
  Context(const llvm::Instruction *c, const Context *p)
      : callSite(c), predContext(p), sz(p == nullptr ? 1 : p->sz + 1), id(0) {}

public:
  // Deterministic sequential ID assigned at interning time (0 = global).
  uint64_t getId() const noexcept { return id; }
  // Get the call instruction at the top of this context's call stack
  const llvm::Instruction *getCallSite() const { return callSite; }
  // Get the depth of this context (number of call sites in the call stack)
  size_t size() const { return sz; }
  // Check if this is the global context (empty call stack)
  bool isGlobalContext() const { return sz == 0; }

  bool operator==(const Context &other) const {
    return callSite == other.callSite && predContext == other.predContext;
  }
  bool operator!=(const Context &other) const { return !(*this == other); }

  // Push a new call site onto the context stack
  // Creates a new context that extends the given context with the new call site
  // Parameters: ctx - the current context, inst - the call instruction
  // Returns: a new context representing the call stack after entering this call
  static const Context *pushContext(const Context *ctx,
                                    const llvm::Instruction *inst);
  // Push a new call site using a ProgramPoint (contains both context and node)
  static const Context *pushContext(const ProgramPoint &);
  // Push a new call site using standard suffix k-CFA: retains the k most recent
  // call sites (dropping the oldest frame when the depth reaches k).
  static const Context *pushContextWithLimit(const Context *ctx,
                                             const llvm::Instruction *inst,
                                             size_t k);
  // Pop the top call site from the context stack
  // Returns the predecessor context (the context before the top call site)
  static const Context *popContext(const Context *ctx);
  // Get the global context (empty call stack, entry point of program)
  static const Context *getGlobalContext();

  // Get all contexts that have been created (useful for debugging and
  // statistics)
  static std::vector<const Context *> getAllContexts();
  friend struct std::hash<Context>;
};

} // namespace context

namespace std {
// Hash function specialization for Context
// Since Contexts are interned in ctxSet, we can use callSite and predContext
// for hashing. The pair (callSite, predContext) uniquely identifies a context.
template <> struct hash<context::Context> {
  size_t operator()(const context::Context &c) const {
    return util::hashPair(c.callSite, c.predContext);
  }
};
} // namespace std
