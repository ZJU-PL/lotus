#pragma once

#include "Analysis/CallGraph/CHA/CHAResolver.h"

#include <vector>

namespace llvm {
class DICompositeType;
} // namespace llvm

namespace lotus {

class RTAResolver : public CHAResolver {
public:
  RTAResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
              const DIBasedTypeHierarchy *TH);

  ~RTAResolver() override = default;

  void resolveVirtualCall(FunctionSetTy &PossibleTargets,
                          const llvm::CallBase *CallSite) override;

  [[nodiscard]] std::string str() const override { return "RTA"; }

  [[nodiscard]] bool
  mutatesHelperAnalysisInformation() const noexcept override {
    return false;
  }

private:
  void resolveAllocatedCompositeTypes();

  std::vector<const llvm::DICompositeType *> AllocatedCompositeTypes;
};

} // namespace lotus
