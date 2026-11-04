#include "Analysis/CallGraph/VTA/VTAResolver.h"
#include "Analysis/CallGraph/RTA/RTAResolver.h"
#include "Analysis/CallGraph/VirtualCallUtils.h"
#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

namespace lotus {

VTAResolver::VTAResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
                         std::unique_ptr<Resolver> BaseRes)
    : Resolver(M, VTP), BaseResolver(std::move(BaseRes)) {
  if (!BaseResolver && M && VTP) {
    BaseResolver = std::make_unique<RTAResolver>(M, VTP, nullptr);
  }

  if (M && VTP && BaseResolver) {
    TAG = vta::computeTypeAssignmentGraph(*M, *VTP, *BaseResolver);
    auto [ComputedSCCs, Order] = vta::computeSCCsAndTopologicalOrder(TAG);
    auto Deps = vta::computeSCCDependencies(TAG, ComputedSCCs);
    TA = vta::propagateTypes(TAG, ComputedSCCs, Deps, Order);
    SCCs = std::move(ComputedSCCs);
  }
}

VTAResolver::VTAResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
                         const CallGraph *BaseCG)
    : VTAResolver(M, VTP,
                  BaseCG ? std::make_unique<PrecomputedResolver>(M, VTP, BaseCG)
                         : nullptr) {}

void VTAResolver::resolveVirtualCall(FunctionSetTy &PossibleTargets,
                                     const llvm::CallBase *CallSite) {
  auto RetrievedVtableIndex = getVFTIndex(CallSite);
  if (!RetrievedVtableIndex.has_value()) {
    return;
  }

  auto VtableIndex = RetrievedVtableIndex.value();
  FunctionSetTy BaseCallees;
  if (BaseResolver) {
    BaseCallees = BaseResolver->resolveIndirectCall(CallSite);
  }

  auto ReceiverIdx = uint32_t(CallSite->hasStructRetAttr());
  if (CallSite->arg_size() > ReceiverIdx) {
    const auto *Receiver = CallSite->getArgOperand(ReceiverIdx);
    if (auto ReceiverNod = TAG.get({vta::Variable{Receiver}})) {
      if (*ReceiverNod < SCCs.SCCOfNode.size()) {
        auto SCC = SCCs.SCCOfNode[*ReceiverNod];
        const auto *ReceiverType = getReceiverType(CallSite);

        if (SCC < TA.TypesPerSCC.size() && VTP) {
          const auto &Types = TA.TypesPerSCC[SCC];
          for (auto Ty : Types) {
            if (const auto *DITy = Ty.dyn_cast<const llvm::DIType *>()) {
              if (const auto *Fun = getNonPureVirtualVFTEntry(
                      DITy, VtableIndex, CallSite, *VTP, ReceiverType)) {
                if (isConsistentCall(CallSite, Fun) &&
                    (BaseCallees.empty() || BaseCallees.contains(Fun))) {
                  PossibleTargets.insert(Fun);
                }
              }
            }
          }
        }
      }
    }
  }

  auto *CalledOp = CallSite->getCalledOperand()->stripPointerCastsAndAliases();
  if (auto TNId = TAG.get({vta::Variable{CalledOp}})) {
    if (*TNId < SCCs.SCCOfNode.size()) {
      auto SCC = SCCs.SCCOfNode[*TNId];
      if (SCC < TA.TypesPerSCC.size()) {
        const auto &Types = TA.TypesPerSCC[SCC];
        for (auto Ty : Types) {
          if (const auto *Fun = Ty.dyn_cast<const llvm::Function *>()) {
            if (isConsistentCall(CallSite, Fun) &&
                (BaseCallees.empty() || BaseCallees.contains(Fun))) {
              PossibleTargets.insert(Fun);
            }
          }
        }
      }
    }
  }

  if (PossibleTargets.empty()) {
    PossibleTargets = std::move(BaseCallees);
  }
}

void VTAResolver::resolveFunctionPointer(FunctionSetTy &PossibleTargets,
                                         const llvm::CallBase *CallSite) {
  FunctionSetTy BaseCallees;
  if (BaseResolver) {
    BaseCallees = BaseResolver->resolveIndirectCall(CallSite);
  }

  auto *CalledOp = CallSite->getCalledOperand()->stripPointerCastsAndAliases();
  if (auto TNId = TAG.get({vta::Variable{CalledOp}})) {
    if (*TNId < SCCs.SCCOfNode.size()) {
      auto SCC = SCCs.SCCOfNode[*TNId];
      if (SCC < TA.TypesPerSCC.size()) {
        const auto &Types = TA.TypesPerSCC[SCC];
        for (auto Ty : Types) {
          if (const auto *Fun = Ty.dyn_cast<const llvm::Function *>()) {
            if (isConsistentCall(CallSite, Fun) &&
                (BaseCallees.empty() || BaseCallees.contains(Fun))) {
              PossibleTargets.insert(Fun);
            }
          }
        }
      }
    }
  }

  if (PossibleTargets.empty()) {
    PossibleTargets = std::move(BaseCallees);
  }
}

} // namespace lotus
