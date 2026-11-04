#include "Analysis/CallGraph/CHA/CHAResolver.h"
#include "Analysis/CallGraph/VirtualCallUtils.h"
#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"

#include "llvm/IR/Function.h"

namespace lotus {

CHAResolver::CHAResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
                         const DIBasedTypeHierarchy *TH)
    : Resolver(M, VTP), TH(TH) {
  if (!TH && M) {
    OwnedTH = std::make_unique<DIBasedTypeHierarchy>(*M);
    this->TH = OwnedTH.get();
  }
}

CHAResolver::~CHAResolver() = default;

void CHAResolver::resolveVirtualCall(FunctionSetTy &PossibleTargets,
                                     const llvm::CallBase *CallSite) {
  auto RetrievedVtableIndex = getVFTIndex(CallSite);
  if (!RetrievedVtableIndex.has_value()) {
    return;
  }

  auto VtableIndex = RetrievedVtableIndex.value();
  const auto *ReceiverTy = getReceiverType(CallSite);
  if (!ReceiverTy || !TH || !VTP) {
    return;
  }

  auto FallbackTys = TH->getSubTypes(ReceiverTy);
  for (const auto &FallbackTy : FallbackTys) {
    const auto *Target = getNonPureVirtualVFTEntry(
        FallbackTy, VtableIndex, CallSite, *VTP, ReceiverTy);
    if (Target && isConsistentCall(CallSite, Target)) {
      PossibleTargets.insert(Target);
    }
  }
}

} // namespace lotus
