#pragma once

#include "Analysis/CallGraph/Resolver.h"
#include "Analysis/CallGraph/VTA/SCCGeneric.h"
#include "Analysis/CallGraph/VTA/TypeAssignmentGraph.h"
#include "Analysis/CallGraph/VTA/TypePropagator.h"

#include <memory>

namespace lotus {

class VTAResolver : public Resolver {
public:
  explicit VTAResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
                       std::unique_ptr<Resolver> BaseRes = nullptr);

  explicit VTAResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
                       const CallGraph *BaseCG);

  ~VTAResolver() override = default;

  [[nodiscard]] std::string str() const override { return "VTA"; }

  [[nodiscard]] bool
  mutatesHelperAnalysisInformation() const noexcept override {
    return false;
  }

  void resolveVirtualCall(FunctionSetTy &PossibleTargets,
                          const llvm::CallBase *CallSite) override;

  void resolveFunctionPointer(FunctionSetTy &PossibleTargets,
                              const llvm::CallBase *CallSite) override;

private:
  std::unique_ptr<Resolver> BaseResolver;
  vta::TypeAssignmentGraph TAG;
  vta::SCCHolder SCCs;
  vta::TypeAssignment TA;
};

} // namespace lotus
