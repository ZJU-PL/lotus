#include "Analysis/CallGraph/RTA/RTAResolver.h"
#include "Analysis/CallGraph/AllocatedTypes.h"
#include "Analysis/CallGraph/VirtualCallUtils.h"
#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"

namespace lotus {

RTAResolver::RTAResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
                         const DIBasedTypeHierarchy *TH)
    : CHAResolver(M, VTP, TH) {
  resolveAllocatedCompositeTypes();
}

void RTAResolver::resolveAllocatedCompositeTypes() {
  if (!AllocatedCompositeTypes.empty() || !M) {
    return;
  }
  AllocatedCompositeTypes = collectAllocatedTypes(*M);
}

void RTAResolver::resolveVirtualCall(FunctionSetTy &PossibleTargets,
                                     const llvm::CallBase *CallSite) {
  auto RetrievedVtableIndex = getVFTIndex(CallSite);
  if (!RetrievedVtableIndex.has_value()) {
    return;
  }

  auto VtableIndex = RetrievedVtableIndex.value();
  const auto *ReceiverType = getReceiverType(CallSite);
  if (!ReceiverType || !TH || !VTP) {
    return;
  }

  auto ReachableTypes = TH->getSubTypes(ReceiverType);
  for (const auto *PossibleType : AllocatedCompositeTypes) {
    if (ReachableTypes.find(PossibleType) != ReachableTypes.end()) {
      const auto *Target = getNonPureVirtualVFTEntry(
          PossibleType, VtableIndex, CallSite, *VTP, ReceiverType);
      if (Target && isConsistentCall(CallSite, Target)) {
        PossibleTargets.insert(Target);
      }
    }
  }

  if (PossibleTargets.empty()) {
    CHAResolver::resolveVirtualCall(PossibleTargets, CallSite);
  }
}

} // namespace lotus
