/** @file DeclarationSummaryProvider.h @brief Declaration-based purity summary provider for external functions. */
#pragma once

#include "Analysis/Summary/Purity/PuritySummary.h"

#include "llvm/ADT/StringRef.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace llvm {
class CallBase;
class Function;
class Module;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace purity {

class MemorySSAPuritySummaryProvider;

class DeclarationSummaryProvider {
public:
  virtual ~DeclarationSummaryProvider() = default;

  virtual llvm::StringRef getName() const = 0;

  virtual std::optional<FunctionEffectSummary>
  getSummary(const llvm::Function &function,
             const llvm::CallBase *callSite) const = 0;
};

class ExternalPuritySummaryProvider final : public DeclarationSummaryProvider {
public:
  ExternalPuritySummaryProvider() = default;

  llvm::StringRef getName() const override;

  std::optional<FunctionEffectSummary>
  getSummary(const llvm::Function &function,
             const llvm::CallBase *callSite) const override;

  void setSummary(llvm::StringRef functionName, FunctionEffectSummary summary);

private:
  std::unordered_map<std::string, FunctionEffectSummary> summaries_;
};

std::vector<std::shared_ptr<const DeclarationSummaryProvider>>
createDefaultDeclarationSummaryProviders(
    llvm::Module &module,
    const MemorySSAPuritySummaryProvider *memorySSAProvider,
    const std::vector<std::shared_ptr<const DeclarationSummaryProvider>>
        &externalProviders = {});

} // namespace purity
} // namespace analysis
} // namespace lotus
