// Implementation of Standard Suffix k-CFA Context Sensitivity (KLimitContext).
//
// KLimitContext maintains the k most recent call sites on the call string
// (sliding-window suffix truncation):
// - k == 0: always returns the global (empty) context (0-CFA).
// - depth < k: appends `inst` to `ctx`.
// - depth >= k: drops the oldest call site(s) and keeps the (k - 1) most
//   recent call sites from `ctx` followed by `inst`.

#include "Alias/InclusionBased/TPA/Context/KLimitContext.h"

#include "Alias/InclusionBased/TPA/Context/ProgramPoint.h"

using namespace llvm;

namespace context {

const Context *KLimitContext::pushContext(const ProgramPoint &pp) {
  return pushContext(pp.getContext(), pp.getInstruction());
}

const Context *KLimitContext::pushContext(const Context *ctx,
                                          const Instruction *inst) {
  return Context::pushContextWithLimit(ctx, inst, defaultLimit);
}

} // namespace context
