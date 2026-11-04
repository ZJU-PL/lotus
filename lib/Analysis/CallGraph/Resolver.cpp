#include "Analysis/CallGraph/Resolver.h"
#include "Analysis/CallGraph/CHA/CHAResolver.h"
#include "Analysis/CallGraph/CallGraph.h"
#include "Analysis/CallGraph/OTF/OTFResolver.h"
#include "Analysis/CallGraph/RTA/RTAResolver.h"
#include "Analysis/CallGraph/VTA/VTAResolver.h"
#include "Analysis/CallGraph/VirtualCallUtils.h"
#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

namespace lotus {

const llvm::Function *getNonPureVirtualVFTEntry(
    const llvm::DIType *T, unsigned Idx, const llvm::CallBase *CallSite,
    const LLVMVFTableProvider &VTP, const llvm::DIType *ReceiverType) {
  const auto &Indices = VTP.getVTableIndexInHierarchy(T, ReceiverType);
  if (Indices.empty()) {
    return nullptr;
  }
  auto VTIndex = *Indices.begin();

  if (const auto *VT = VTP.getVFTableOrNull(T, VTIndex)) {
    const auto *Target = VT->getFunction(Idx);
    if (Target &&
        Target->getName() != DIBasedTypeHierarchy::PureVirtualCallName &&
        isConsistentCall(CallSite, Target)) {
      return Target;
    }
  }

  return nullptr;
}

Resolver::Resolver(const llvm::Module *M, const LLVMVFTableProvider *VTP)
    : M(M), VTP(VTP) {}

void Resolver::handlePossibleTargets(const llvm::CallBase *CallSite,
                                     FunctionSetTy &PossibleTargets) {}

auto Resolver::resolveIndirectCall(const llvm::CallBase *CallSite)
    -> FunctionSetTy {
  FunctionSetTy PossibleTargets;
  if (VTP && isVirtualCall(CallSite, *VTP)) {
    resolveVirtualCall(PossibleTargets, CallSite);
  } else {
    resolveFunctionPointer(PossibleTargets, CallSite);
  }
  return PossibleTargets;
}

llvm::ArrayRef<const llvm::Function *> Resolver::getAddressTakenFunctions() {
  if (!AddressTakenFunctions) {
    auto &ATF = AddressTakenFunctions.emplace();
    if (M) {
      ATF.reserve(M->size() / 2);
      for (const auto &F : *M) {
        if (isAddressTakenFunction(&F)) {
          ATF.push_back(&F);
        }
      }
    }
  }

  return *AddressTakenFunctions;
}

void Resolver::resolveFunctionPointer(FunctionSetTy &PossibleTargets,
                                      const llvm::CallBase *CallSite) {
  for (const auto *F : getAddressTakenFunctions()) {
    if (isConsistentCall(CallSite, F)) {
      PossibleTargets.insert(F);
    }
  }
}

void PrecomputedResolver::resolveVirtualCall(FunctionSetTy &PossibleTargets,
                                             const llvm::CallBase *CallSite) {
  resolveFunctionPointer(PossibleTargets, CallSite);
}

void PrecomputedResolver::resolveFunctionPointer(
    FunctionSetTy &PossibleTargets, const llvm::CallBase *CallSite) {
  if (BaseCG) {
    auto Callees = BaseCG->getCalleesOfCallAt(CallSite);
    PossibleTargets.insert(Callees.begin(), Callees.end());
  }
}

std::unique_ptr<Resolver>
Resolver::create(CallGraphAnalysisType Ty, const llvm::Module *M,
                 const LLVMVFTableProvider *VTP,
                 const DIBasedTypeHierarchy *TH) {
  switch (Ty) {
  case CallGraphAnalysisType::NORESOLVE:
    return std::make_unique<NOResolver>(M, VTP);
  case CallGraphAnalysisType::CHA:
    return std::make_unique<CHAResolver>(M, VTP, TH);
  case CallGraphAnalysisType::RTA:
    return std::make_unique<RTAResolver>(M, VTP, TH);
  case CallGraphAnalysisType::VTA:
    return std::make_unique<VTAResolver>(M, VTP, std::make_unique<RTAResolver>(M, VTP, TH));
  case CallGraphAnalysisType::OTF:
    return std::make_unique<OTFResolver>(M, VTP, TH);
  case CallGraphAnalysisType::Invalid:
    llvm::report_fatal_error("Invalid callgraph algorithm specified");
  }

  llvm_unreachable("All callgraph algorithms should be handled");
}

} // namespace lotus
