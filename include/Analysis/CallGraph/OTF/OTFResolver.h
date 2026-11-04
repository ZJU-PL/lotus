#pragma once

#include "Analysis/CallGraph/Resolver.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <functional>
#include <memory>
#include <vector>

namespace lotus {

class DIBasedTypeHierarchy;
class RTAResolver;

class OTFResolver : public Resolver {
public:
  using AliasSetTy = llvm::DenseSet<const llvm::Value *>;
  using AliasProviderFn =
      std::function<std::vector<const llvm::Value *>(const llvm::Value *,
                                                     const llvm::Instruction *)>;

  OTFResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
              const DIBasedTypeHierarchy *TH = nullptr,
              AliasProviderFn ExternalAliasProvider = nullptr);

  ~OTFResolver() override;

  void handlePossibleTargets(const llvm::CallBase *CallSite,
                             FunctionSetTy &CalleeTargets) override;

  void resolveVirtualCall(FunctionSetTy &PossibleTargets,
                          const llvm::CallBase *CallSite) override;

  void resolveFunctionPointer(FunctionSetTy &PossibleTargets,
                              const llvm::CallBase *CallSite) override;

  [[nodiscard]] std::string str() const override { return "OTF"; }

  [[nodiscard]] bool
  mutatesHelperAnalysisInformation() const noexcept override {
    return true;
  }

  void introduceAlias(const llvm::Value *V1, const llvm::Value *V2);

  [[nodiscard]] AliasSetTy getAliasSet(const llvm::Value *V,
                                       const llvm::Instruction *At = nullptr) const;

private:
  void initializeIntraProceduralAliases();

  const DIBasedTypeHierarchy *TH = nullptr;
  std::unique_ptr<RTAResolver> FallbackRTA;
  AliasProviderFn ExternalAliasProvider;
  mutable llvm::DenseMap<const llvm::Value *, AliasSetTy> AliasGraph;
};

} // namespace lotus
