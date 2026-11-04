#pragma once

#include "Analysis/CallGraph/Resolver.h"

#include <memory>

namespace lotus {

class DIBasedTypeHierarchy;

class CHAResolver : public Resolver {
public:
  CHAResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
              const DIBasedTypeHierarchy *TH);

  ~CHAResolver() override;

  void resolveVirtualCall(FunctionSetTy &PossibleTargets,
                          const llvm::CallBase *CallSite) override;

  [[nodiscard]] std::string str() const override { return "CHA"; }

  [[nodiscard]] bool
  mutatesHelperAnalysisInformation() const noexcept override {
    return false;
  }

protected:
  const DIBasedTypeHierarchy *TH = nullptr;
  std::unique_ptr<DIBasedTypeHierarchy> OwnedTH;
};

} // namespace lotus
