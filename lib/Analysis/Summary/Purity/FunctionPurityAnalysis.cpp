/*
Purity analysis for LLVM IR functions.

This classifier targets source-level attribute candidates:
- Const: no reachable-memory reads or writes, and no observable side effects.
- Pure: may read reachable memory, but does not write reachable memory and has
  no observable side effects.
- Impure: writes reachable memory or has observable side effects.
- Unknown: current summaries are insufficient to prove const/pure safely.
*/
#include "Analysis/Summary/Purity/DeclarationSummaryProvider.h"
#include "Analysis/Summary/Purity/FunctionPurityAnalysis.h"

#include "Analysis/Summary/Purity/MemorySSAPuritySummary.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <utility>

namespace lotus::analysis::purity {

using namespace llvm;

namespace {

bool isBenignMemoryIntrinsic(const IntrinsicInst &II) {
  switch (II.getIntrinsicID()) {
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_label:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::invariant_start:
  case Intrinsic::invariant_end:
  case Intrinsic::assume:
  case Intrinsic::expect:
    return true;
  default:
    return false;
  }
}

bool isShadowMemCall(const CallBase &call) {
  const Function *callee = call.getCalledFunction();
  return callee && callee->getName().startswith("shadow.mem");
}

FunctionEffectSummary constSummary(
    SummarySource source = SummarySource::InternalAnalysis,
    SummaryConfidence confidence = SummaryConfidence::High) {
  FunctionEffectSummary summary;
  summary.source = source;
  summary.confidence = confidence;
  summary.fromMemorySSA = source == SummarySource::MemorySSA;
  return summary;
}

FunctionEffectSummary pureSummary(
    SummarySource source = SummarySource::InternalAnalysis,
    SummaryConfidence confidence = SummaryConfidence::High) {
  FunctionEffectSummary summary = constSummary(source, confidence);
  summary.readsReachableMemory = true;
  return summary;
}

FunctionEffectSummary impureSummary(
    bool readsMemory = false,
    SummarySource source = SummarySource::InternalAnalysis,
    SummaryConfidence confidence = SummaryConfidence::High) {
  FunctionEffectSummary summary = constSummary(source, confidence);
  summary.readsReachableMemory = readsMemory;
  summary.writesReachableMemory = true;
  return summary;
}

FunctionEffectSummary unknownSummary(
    bool readsMemory = false,
    SummarySource source = SummarySource::ConservativeFallback,
    StringRef dependency = {},
    SummaryConfidence confidence = SummaryConfidence::High) {
  FunctionEffectSummary summary = constSummary(source, confidence);
  summary.readsReachableMemory = readsMemory;
  summary.hasUnknownEffects = true;
  summary.addDependency(dependency);
  return summary;
}

bool hasObservableCallEffects(const CallBase &call) {
  if (call.isInlineAsm()) {
    return true;
  }
  if (call.hasFnAttr(Attribute::ReturnsTwice)) {
    return true;
  }
  if (call.isConvergent()) {
    return true;
  }
  return false;
}

bool hasExplicitMemoryAttribute(const Function &function) {
  return function.hasFnAttribute(Attribute::ReadNone) ||
         function.hasFnAttribute(Attribute::ReadOnly) ||
         function.doesNotAccessMemory() || function.onlyReadsMemory();
}

void mergeDependencies(FunctionEffectSummary &target,
                       const FunctionEffectSummary &source) {
  for (const std::string &dependency : source.dependsOn) {
    target.addDependency(dependency);
  }
}

bool summariesEqual(const FunctionEffectSummary &lhs,
                    const FunctionEffectSummary &rhs) {
  return lhs.readsReachableMemory == rhs.readsReachableMemory &&
         lhs.writesReachableMemory == rhs.writesReachableMemory &&
         lhs.hasObservableSideEffects == rhs.hasObservableSideEffects &&
         lhs.hasUnknownEffects == rhs.hasUnknownEffects &&
         lhs.fromMemorySSA == rhs.fromMemorySSA &&
         lhs.source == rhs.source && lhs.confidence == rhs.confidence &&
         lhs.dependsOn == rhs.dependsOn;
}

} // namespace

FunctionPurityAnalysis::FunctionPurityAnalysis(Module &module,
                                               MemorySSAMode memorySSAMode)
    : FunctionPurityAnalysis(module,
                             FunctionPurityAnalysisOptions{memorySSAMode}) {}

FunctionPurityAnalysis::FunctionPurityAnalysis(
    Module &module, FunctionPurityAnalysisOptions options)
    : module_(module), options_(std::move(options)) {
  if (options_.memorySSAMode == MemorySSAMode::UseIfAvailable) {
    memorySSAProvider_ =
        std::make_unique<MemorySSAPuritySummaryProvider>(module_);
  }

  if (options_.declarationSummaryProviders.empty()) {
    declarationSummaryProviders_ = createDefaultDeclarationSummaryProviders(
        module_, memorySSAProvider_.get(), options_.externalSummaryProviders);
  } else {
    declarationSummaryProviders_ = options_.declarationSummaryProviders;
  }
}

FunctionPurityAnalysis::~FunctionPurityAnalysis() = default;

void FunctionPurityAnalysis::run() {
  summaries_.clear();

  for (Function &function : module_) {
    summaries_[&function] =
        function.isDeclaration() ? classifyDeclaration(function)
                                 : initialSummary(function);
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (Function &function : module_) {
      if (function.isDeclaration()) {
        continue;
      }
      const FunctionEffectSummary next = analyzeFunction(function);
      auto it = summaries_.find(&function);
      if (it == summaries_.end() || !summariesEqual(it->second, next)) {
        summaries_[&function] = next;
        changed = true;
      }
    }
  }

  ran_ = true;
}

PurityKind FunctionPurityAnalysis::getPurity(const Function *function) const {
  return getEffects(function).getPurityKind();
}

PurityKind FunctionPurityAnalysis::getCallPurity(const CallBase &call) const {
  return classifyCall(call).getPurityKind();
}

FunctionEffectSummary
FunctionPurityAnalysis::getEffects(const Function *function) const {
  if (!function || !ran_) {
    return unknownSummary();
  }
  auto it = summaries_.find(function);
  return it == summaries_.end() ? unknownSummary() : it->second;
}

bool FunctionPurityAnalysis::hasMemorySSASummaries() const {
  return memorySSAProvider_ && memorySSAProvider_->hasInstrumentedIR();
}

bool FunctionPurityAnalysis::isConst(const Function *function) const {
  return getPurity(function) == PurityKind::Const;
}

bool FunctionPurityAnalysis::isPure(const Function *function) const {
  return getPurity(function) == PurityKind::Pure;
}

bool FunctionPurityAnalysis::isAtMostPure(const Function *function) const {
  const PurityKind kind = getPurity(function);
  return kind == PurityKind::Const || kind == PurityKind::Pure;
}

bool FunctionPurityAnalysis::isKnown(const Function *function) const {
  return getPurity(function) != PurityKind::Unknown;
}

FunctionEffectSummary
FunctionPurityAnalysis::initialSummary(const Function &function) const {
  if (hasMemorySSASummaries()) {
    if (auto summary = memorySSAProvider_->getFunctionSummary(function)) {
      if (summary->writesReachableMemory) {
        return impureSummary(summary->readsReachableMemory,
                             SummarySource::MemorySSA);
      }
      return summary->readsReachableMemory ? pureSummary(SummarySource::MemorySSA)
                                           : constSummary(SummarySource::MemorySSA);
    }
  }
  return constSummary();
}

FunctionEffectSummary
FunctionPurityAnalysis::analyzeFunction(const Function &function) const {
  FunctionEffectSummary result = initialSummary(function);
  bool usedNonLocalCalleeSummary = false;
  bool sawLocalEffect = false;

  for (const Instruction &inst : instructions(function)) {
    if (isa<ResumeInst>(inst) || isa<CleanupReturnInst>(inst) ||
        isa<CatchReturnInst>(inst) || isa<FenceInst>(inst) ||
        isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst) ||
        isa<VAArgInst>(inst)) {
      result.hasObservableSideEffects = true;
      result.source = SummarySource::InternalAnalysis;
      return result;
    }

    if (const auto *load = dyn_cast<LoadInst>(&inst)) {
      if (load->isVolatile() || load->isAtomic()) {
        result.hasObservableSideEffects = true;
        result.source = SummarySource::InternalAnalysis;
        return result;
      }
      if (!hasMemorySSASummaries()) {
        result = merge(result, pureSummary());
      }
      sawLocalEffect = true;
      continue;
    }

    if (const auto *store = dyn_cast<StoreInst>(&inst)) {
      if (store->isVolatile() || store->isAtomic()) {
        result.hasObservableSideEffects = true;
        result.source = SummarySource::InternalAnalysis;
        return result;
      }
      if (!hasMemorySSASummaries()) {
        return impureSummary();
      }
      sawLocalEffect = true;
      continue;
    }

    if (const auto *call = dyn_cast<CallBase>(&inst)) {
      if (isShadowMemCall(*call)) {
        continue;
      }
      usedNonLocalCalleeSummary = true;
      result = merge(result, classifyCall(*call));
      if (result.getPurityKind() == PurityKind::Impure) {
        return result;
      }
      continue;
    }

    if (inst.mayReadFromMemory() && !hasMemorySSASummaries()) {
      result = merge(result, pureSummary());
      sawLocalEffect = true;
    }
  }

  if (!result.dependsOn.empty()) {
    result.source = SummarySource::Propagated;
  } else if (usedNonLocalCalleeSummary && !sawLocalEffect &&
             result.source != SummarySource::MemorySSA) {
    result.source = SummarySource::Propagated;
  }

  return result;
}

FunctionEffectSummary FunctionPurityAnalysis::classifyDeclaration(
    const Function &function, const CallBase *callSite) const {
  if (function.isIntrinsic()) {
    return classifyIntrinsic(function);
  }

  for (const auto &provider : declarationSummaryProviders_) {
    if (!provider) {
      continue;
    }
    if (auto summary = provider->getSummary(function, callSite)) {
      return *summary;
    }
  }

  return unknownSummary(false, SummarySource::ConservativeFallback,
                        function.getName());
}

FunctionEffectSummary
FunctionPurityAnalysis::classifyCall(const CallBase &call) const {
  if (hasObservableCallEffects(call)) {
    FunctionEffectSummary summary = impureSummary();
    summary.hasObservableSideEffects = true;
    return summary;
  }

  if (const auto *II = dyn_cast<IntrinsicInst>(&call)) {
    if (isBenignMemoryIntrinsic(*II)) {
      return constSummary();
    }
  }

  const Function *callee = call.getCalledFunction();
  if (!callee) {
    if (call.doesNotAccessMemory()) {
      return constSummary(SummarySource::LocalAttributes);
    }
    if (call.onlyReadsMemory()) {
      return pureSummary(SummarySource::LocalAttributes);
    }
    return unknownSummary(false, SummarySource::ConservativeFallback);
  }

  if (callee == call.getFunction()) {
    return constSummary();
  }

  if (callee->isIntrinsic()) {
    return classifyIntrinsic(*callee);
  }

  if (callee->isDeclaration()) {
    return classifyDeclaration(*callee, &call);
  }

  auto it = summaries_.find(callee);
  return it == summaries_.end() ? unknownSummary() : it->second;
}

FunctionEffectSummary
FunctionPurityAnalysis::classifyIntrinsic(const Function &function) const {
  if (!function.isIntrinsic()) {
    return unknownSummary();
  }

  switch (function.getIntrinsicID()) {
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_label:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::invariant_start:
  case Intrinsic::invariant_end:
  case Intrinsic::assume:
  case Intrinsic::expect:
    return constSummary(SummarySource::InternalAnalysis);
  case Intrinsic::memcpy:
  case Intrinsic::memmove:
  case Intrinsic::memset:
  case Intrinsic::vastart:
  case Intrinsic::vaend:
    return impureSummary();
  default:
    break;
  }

  if ((function.doesNotAccessMemory() ||
       function.hasFnAttribute(Attribute::ReadNone))) {
    return constSummary(SummarySource::LocalAttributes);
  }
  if ((function.onlyReadsMemory() ||
       function.hasFnAttribute(Attribute::ReadOnly))) {
    return pureSummary(SummarySource::LocalAttributes);
  }

  return unknownSummary();
}

FunctionEffectSummary FunctionPurityAnalysis::merge(
    const FunctionEffectSummary &lhs, const FunctionEffectSummary &rhs) {
  FunctionEffectSummary merged;
  merged.readsReachableMemory =
      lhs.readsReachableMemory || rhs.readsReachableMemory;
  merged.writesReachableMemory =
      lhs.writesReachableMemory || rhs.writesReachableMemory;
  merged.hasObservableSideEffects =
      lhs.hasObservableSideEffects || rhs.hasObservableSideEffects;
  merged.hasUnknownEffects = lhs.hasUnknownEffects || rhs.hasUnknownEffects;
  merged.fromMemorySSA = lhs.fromMemorySSA || rhs.fromMemorySSA;
  merged.confidence = minConfidence(lhs.confidence, rhs.confidence);
  merged.source = lhs.source == rhs.source ? lhs.source : SummarySource::Propagated;
  mergeDependencies(merged, lhs);
  mergeDependencies(merged, rhs);
  return merged;
}

} // namespace lotus::analysis::purity
