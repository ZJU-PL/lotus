#include "Alias/UnderApproxAA/UnderApproxAA.h"
#include "Alias/UnderApproxAA/EquivDB.h"
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Instructions.h>
#include <unordered_map>

using namespace llvm;
using namespace UnderApprox;

// ---------------------------------------------------------------------------
// Per-function cache – built lazily, reused by all subsequent queries
// ---------------------------------------------------------------------------

namespace {
using CacheTy =
    std::unordered_map<const Function *, std::unique_ptr<EquivDB>>;
static CacheTy EquivCache;

// Helper to get parent function of a value
const Function *getParentFunction(const Value *V) {
  if (auto *I = dyn_cast<Instruction>(V))
    return I->getParent()->getParent();
  if (auto *A = dyn_cast<Argument>(V))
    return A->getParent();
  return nullptr;
}

} // namespace

UnderApproxAA::UnderApproxAA(Module &M) : _module(M) {}
UnderApproxAA::~UnderApproxAA() {}

// ---------------------------------------------------------------------------
// AAResult interface
// ---------------------------------------------------------------------------

AliasResult UnderApproxAA::alias(const MemoryLocation &L1,
                                 const MemoryLocation &L2) {
  return mustAlias(L1.Ptr, L2.Ptr) ? AliasResult::MustAlias
                                   : AliasResult::NoAlias;
}

bool UnderApproxAA::mustAlias(const Value *V1, const Value *V2) {
  if (!isValidPointerQuery(V1, V2)) return false;

  const Function *F1 = getParentFunction(V1);
  const Function *F2 = getParentFunction(V2);

  // Cross-function queries – fall back to atomic test only (rare in practice).
  if (F1 != F2)
    return false;

  auto &Ptr = EquivCache[F1];
  if (!Ptr) Ptr = std::make_unique<EquivDB>(*const_cast<Function *>(F1));

  return Ptr->mustAlias(V1, V2);
}

bool UnderApproxAA::isValidPointerQuery(const Value *v1,
                                        const Value *v2) const {
  return v1 && v2 && v1->getType()->isPointerTy() &&
         v2->getType()->isPointerTy();
}