#include "Analysis/Summary/Purity/DeclarationSummaryProvider.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"
#include "Analysis/Summary/Purity/MemorySSAPuritySummary.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/IntrinsicInst.h"

namespace lotus::analysis::purity {

using namespace llvm;

namespace {

FunctionEffectSummary constSummary(SummarySource source,
                                   SummaryConfidence confidence =
                                       SummaryConfidence::High) {
  FunctionEffectSummary summary;
  summary.source = source;
  summary.confidence = confidence;
  summary.fromMemorySSA = source == SummarySource::MemorySSA;
  return summary;
}

FunctionEffectSummary pureSummary(SummarySource source,
                                  SummaryConfidence confidence =
                                      SummaryConfidence::High) {
  FunctionEffectSummary summary = constSummary(source, confidence);
  summary.readsReachableMemory = true;
  return summary;
}

FunctionEffectSummary impureSummary(SummarySource source, bool readsMemory,
                                    SummaryConfidence confidence =
                                        SummaryConfidence::High) {
  FunctionEffectSummary summary = pureSummary(source, confidence);
  summary.readsReachableMemory = readsMemory;
  summary.writesReachableMemory = true;
  return summary;
}

FunctionEffectSummary unknownSummary(SummarySource source, bool readsMemory,
                                     StringRef dependency = {},
                                     SummaryConfidence confidence =
                                         SummaryConfidence::High) {
  FunctionEffectSummary summary = constSummary(source, confidence);
  summary.readsReachableMemory = readsMemory;
  summary.hasUnknownEffects = true;
  summary.addDependency(dependency);
  return summary;
}

bool hasExplicitMemoryAttribute(const Function &function) {
  return function.hasFnAttribute(Attribute::ReadNone) ||
         function.hasFnAttribute(Attribute::ReadOnly) ||
         function.doesNotAccessMemory() || function.onlyReadsMemory();
}

bool isShadowMemHelper(const Function &function) {
  return function.getName().startswith("shadow.mem");
}

class LocalAttributePurityProvider final : public DeclarationSummaryProvider {
public:
  llvm::StringRef getName() const override { return "local-attributes"; }

  std::optional<FunctionEffectSummary>
  getSummary(const Function &function, const CallBase *callSite) const override {
    (void)callSite;

    if (isShadowMemHelper(function)) {
      return constSummary(SummarySource::InternalAnalysis);
    }

    if (function.doesNotAccessMemory() ||
        function.hasFnAttribute(Attribute::ReadNone)) {
      return constSummary(SummarySource::LocalAttributes);
    }

    if (function.onlyReadsMemory() ||
        function.hasFnAttribute(Attribute::ReadOnly)) {
      return pureSummary(SummarySource::LocalAttributes);
    }

    return std::nullopt;
  }
};

class BuiltinSpecPurityProvider final : public DeclarationSummaryProvider {
public:
  explicit BuiltinSpecPurityProvider(Module &module) {
    specs_.initialize(module);
  }

  llvm::StringRef getName() const override { return "builtin-spec"; }

  std::optional<FunctionEffectSummary>
  getSummary(const Function &function, const CallBase *callSite) const override {
    (void)callSite;

    if (specs_.isNoEffect(&function)) {
      return constSummary(SummarySource::BuiltinSpec);
    }

    const auto modRef = specs_.getModRefInfo(&function);
    if (modRef.modifiedArgs.empty() && !modRef.modifiesReturn &&
        (!modRef.referencedArgs.empty() || modRef.referencesReturn)) {
      return pureSummary(SummarySource::BuiltinSpec);
    }
    if (!modRef.modifiedArgs.empty() || modRef.modifiesReturn) {
      return impureSummary(
          SummarySource::BuiltinSpec,
          !modRef.referencedArgs.empty() || modRef.referencesReturn);
    }

    return std::nullopt;
  }

private:
  lotus::alias::AliasSpecManager specs_;
};

class MemorySSADeclarationPurityProvider final
    : public DeclarationSummaryProvider {
public:
  explicit MemorySSADeclarationPurityProvider(
      const MemorySSAPuritySummaryProvider *memorySSAProvider)
      : memorySSAProvider_(memorySSAProvider) {}

  llvm::StringRef getName() const override { return "memoryssa"; }

  std::optional<FunctionEffectSummary>
  getSummary(const Function &function, const CallBase *callSite) const override {
    if (!memorySSAProvider_ || !callSite ||
        !memorySSAProvider_->hasInstrumentedIR()) {
      return std::nullopt;
    }

    const auto callSummary = memorySSAProvider_->getCallSummary(*callSite);
    if (!callSummary) {
      return std::nullopt;
    }

    if (callSummary->writesReachableMemory) {
      return impureSummary(SummarySource::MemorySSA,
                           callSummary->readsReachableMemory);
    }

    if (callSummary->readsReachableMemory) {
      if (hasExplicitMemoryAttribute(function)) {
        return pureSummary(SummarySource::MemorySSA);
      }
      return unknownSummary(SummarySource::MemorySSA, true,
                            function.getName());
    }

    if (!hasExplicitMemoryAttribute(function)) {
      return unknownSummary(SummarySource::MemorySSA, false,
                            function.getName());
    }

    return std::nullopt;
  }

private:
  const MemorySSAPuritySummaryProvider *memorySSAProvider_;
};

} // namespace

llvm::StringRef ExternalPuritySummaryProvider::getName() const {
  return "external-summary";
}

std::optional<FunctionEffectSummary>
ExternalPuritySummaryProvider::getSummary(const Function &function,
                                          const CallBase *callSite) const {
  (void)callSite;

  auto it = summaries_.find(function.getName().str());
  if (it == summaries_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void ExternalPuritySummaryProvider::setSummary(llvm::StringRef functionName,
                                               FunctionEffectSummary summary) {
  summary.source = SummarySource::ExternalSummary;
  if (summary.confidence == SummaryConfidence::High) {
    summary.confidence = SummaryConfidence::Medium;
  }
  summary.addDependency(functionName);
  summaries_[functionName.str()] = std::move(summary);
}

std::vector<std::shared_ptr<const DeclarationSummaryProvider>>
createDefaultDeclarationSummaryProviders(
    Module &module, const MemorySSAPuritySummaryProvider *memorySSAProvider,
    const std::vector<std::shared_ptr<const DeclarationSummaryProvider>>
        &externalProviders) {
  std::vector<std::shared_ptr<const DeclarationSummaryProvider>> providers;
  providers.push_back(std::make_shared<LocalAttributePurityProvider>());
  providers.push_back(std::make_shared<BuiltinSpecPurityProvider>(module));
  providers.push_back(
      std::make_shared<MemorySSADeclarationPurityProvider>(memorySSAProvider));
  providers.insert(providers.end(), externalProviders.begin(),
                   externalProviders.end());
  return providers;
}

} // namespace lotus::analysis::purity
