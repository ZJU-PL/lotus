#pragma once

#include "Analysis/CallGraph/CallGraphAnalysisType.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"
#include "Analysis/CallGraph/VirtualCallUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class CallBase;
class Function;
class Instruction;
class Module;
class DIType;
} // namespace llvm

namespace lotus {

class DIBasedTypeHierarchy;
class CallGraph;

const llvm::Function *getNonPureVirtualVFTEntry(
    const llvm::DIType *T, unsigned Idx, const llvm::CallBase *CallSite,
    const LLVMVFTableProvider &VTP, const llvm::DIType *ReceiverType);

class Resolver {
public:
  using FunctionSetTy = llvm::SmallDenseSet<const llvm::Function *, 4>;

  Resolver(const llvm::Module *M, const LLVMVFTableProvider *VTP);
  virtual ~Resolver() = default;

  virtual void handlePossibleTargets(const llvm::CallBase *CallSite,
                                     FunctionSetTy &PossibleTargets);

  [[nodiscard]] virtual FunctionSetTy
  resolveIndirectCall(const llvm::CallBase *CallSite);

  virtual void resolveVirtualCall(FunctionSetTy &PossibleTargets,
                                  const llvm::CallBase *CallSite) = 0;

  virtual void resolveFunctionPointer(FunctionSetTy &PossibleTargets,
                                      const llvm::CallBase *CallSite);

  [[nodiscard]] virtual std::string str() const = 0;

  [[nodiscard]] virtual bool mutatesHelperAnalysisInformation() const noexcept {
    return false;
  }

  [[nodiscard]] llvm::ArrayRef<const llvm::Function *>
  getAddressTakenFunctions();

  static std::unique_ptr<Resolver>
  create(CallGraphAnalysisType Ty, const llvm::Module *M,
         const LLVMVFTableProvider *VTP, const DIBasedTypeHierarchy *TH);

protected:
  const llvm::Module *M = nullptr;
  const LLVMVFTableProvider *VTP = nullptr;
  std::optional<std::vector<const llvm::Function *>> AddressTakenFunctions;
};

class NOResolver : public Resolver {
public:
  NOResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP)
      : Resolver(M, VTP) {}

  void resolveVirtualCall(FunctionSetTy &PossibleTargets,
                          const llvm::CallBase *CallSite) override {}

  void resolveFunctionPointer(FunctionSetTy &PossibleTargets,
                              const llvm::CallBase *CallSite) override {}

  [[nodiscard]] std::string str() const override { return "NORESOLVE"; }
};

class PrecomputedResolver : public Resolver {
public:
  PrecomputedResolver(const llvm::Module *M, const LLVMVFTableProvider *VTP,
                      const CallGraph *BaseCG)
      : Resolver(M, VTP), BaseCG(BaseCG) {}

  void resolveVirtualCall(FunctionSetTy &PossibleTargets,
                          const llvm::CallBase *CallSite) override;

  void resolveFunctionPointer(FunctionSetTy &PossibleTargets,
                              const llvm::CallBase *CallSite) override;

  [[nodiscard]] std::string str() const override { return "Precomputed"; }

private:
  const CallGraph *BaseCG = nullptr;
};

} // namespace lotus
