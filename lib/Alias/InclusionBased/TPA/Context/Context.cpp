// Implementation of the base Context class.
//
// A Context represents the calling history (call stack) of a function
// execution. In TPA, contexts are immutable and interned (deduplicated).
//
// Design:
// - Linked-List Structure: Each Context holds a reference to its predecessor
// (caller's context)
//   and the call instruction that created this context.
// - Interning: A global `ctxSet` stores all unique contexts. Pointers returned
// by
//   `pushContext` are unique representatives, allowing fast pointer equality
//   checks.
// - Global Context: A special context root for the entry point of the program.

#include "Alias/InclusionBased/TPA/Context/Context.h"

#include "Alias/InclusionBased/TPA/Context/ProgramPoint.h"

using namespace llvm;

#include <mutex>

#include <llvm/ADT/SmallVector.h>

namespace context {
namespace {
std::mutex &contextRegistryMutex() {
  static std::mutex mutex;
  return mutex;
}
uint64_t nextContextId = 1;
} // namespace

const Context *Context::insertUnlocked(const Context &candidate,
                                       bool isGlobal) {
  auto res = ctxSet.insert(candidate);
  if (res.second && !isGlobal)
    res.first->id = nextContextId++;
  return &(*res.first);
}

// Pushes a new context frame onto the current context stack.
// This version takes a ProgramPoint, extracting the context and instruction
// from it.
const Context *Context::pushContext(const ProgramPoint &pp) {
  return pushContext(pp.getContext(), pp.getInstruction());
}

// Pushes a new context frame.
// Parameters:
//   ctx - The current (caller's) context.
//   inst - The call instruction (call site) invoking the function.
// Returns: A pointer to the unique interned Context object representing the new
// state.
const Context *Context::pushContext(const Context *ctx,
                                    const Instruction *inst) {
  std::lock_guard<std::mutex> lock(contextRegistryMutex());
  return insertUnlocked(Context(inst, ctx), false);
}

// Pushes a new call site while keeping at most the k most recent call sites
// (standard suffix k-CFA call-string truncation).
const Context *Context::pushContextWithLimit(const Context *ctx,
                                             const Instruction *inst,
                                             size_t k) {
  std::lock_guard<std::mutex> lock(contextRegistryMutex());
  auto getGlobalUnlocked = []() -> const Context * {
    return insertUnlocked(Context(), true);
  };
  auto pushUnlocked = [](const Context *base,
                         const Instruction *cs) -> const Context * {
    return insertUnlocked(Context(cs, base), false);
  };

  if (k == 0)
    return getGlobalUnlocked();
  if (k == 1)
    return pushUnlocked(getGlobalUnlocked(), inst);
  if (ctx->sz < k)
    return pushUnlocked(ctx, inst);

  // Retain the (k - 1) most recent call sites from `ctx`, then append `inst`.
  llvm::SmallVector<const Instruction *, 8> suffix;
  suffix.reserve(k - 1);
  const Context *cur = ctx;
  for (size_t i = 0; i < k - 1; ++i) {
    assert(cur != nullptr && cur->sz > 0);
    suffix.push_back(cur->callSite);
    cur = cur->predContext;
  }

  const Context *res = getGlobalUnlocked();
  for (auto it = suffix.rbegin(), end = suffix.rend(); it != end; ++it)
    res = pushUnlocked(res, *it);
  return pushUnlocked(res, inst);
}

// Pops the most recent context frame.
// Returns the predecessor context (the caller's context).
const Context *Context::popContext(const Context *ctx) {
  assert(ctx->sz != 0 && "Trying to pop an empty context");
  return ctx->predContext;
}

// Retrieves the singleton Global Context.
// Used for global variables and the entry point of the analysis.
const Context *Context::getGlobalContext() {
  std::lock_guard<std::mutex> lock(contextRegistryMutex());
  return insertUnlocked(Context(), true);
}

// Helper to retrieve all created contexts.
// Useful for debugging or statistics.
std::vector<const Context *> Context::getAllContexts() {
  std::lock_guard<std::mutex> lock(contextRegistryMutex());
  std::vector<const Context *> ret;
  ret.reserve(ctxSet.size());

  for (auto const &ctx : ctxSet)
    ret.push_back(&ctx);

  return ret;
}

} // namespace context
