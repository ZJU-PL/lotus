#include "Analysis/Summary/Purity/PurityUnknownImpactPass.h"

#include "Analysis/Summary/Purity/FunctionPurityAnalysis.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace lotus::analysis::purity {

using namespace llvm;

namespace {

bool isShadowMemHelperName(StringRef symbolName) {
  return symbolName.startswith("shadow.mem");
}

void sortImpacts(std::vector<UnknownCalleeImpact> &impacts) {
  std::sort(impacts.begin(), impacts.end(),
            [](const UnknownCalleeImpact &lhs,
               const UnknownCalleeImpact &rhs) {
              if (lhs.transitiveCallerCount != rhs.transitiveCallerCount) {
                return lhs.transitiveCallerCount > rhs.transitiveCallerCount;
              }
              if (lhs.directCallerCount != rhs.directCallerCount) {
                return lhs.directCallerCount > rhs.directCallerCount;
              }
              return lhs.symbolName < rhs.symbolName;
            });
}

} // namespace

UnknownCalleeImpactAnalyzer::UnknownCalleeImpactAnalyzer(
    PurityUnknownImpactPassOptions options)
    : options_(std::move(options)) {}

std::vector<UnknownCalleeImpact>
UnknownCalleeImpactAnalyzer::rankUnknownCallees(Module &module) const {
  FunctionPurityAnalysisOptions analysisOptions;
  analysisOptions.externalSummaryProviders = options_.externalSummaryProviders;

  FunctionPurityAnalysis analysis(module, analysisOptions);
  analysis.run();

  std::unordered_map<std::string, SmallPtrSet<const Function *, 8>> directCallers;
  std::unordered_map<std::string, SmallPtrSet<const Function *, 8>>
      transitiveCallers;

  for (Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }

    if (analysis.getPurity(&function) == PurityKind::Unknown) {
      const auto effects = analysis.getEffects(&function);
      for (const std::string &dependency : effects.dependsOn) {
        if (isShadowMemHelperName(dependency)) {
          continue;
        }
        transitiveCallers[dependency].insert(&function);
      }
    }

    for (const Instruction &inst : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }

      const Function *callee = call->getCalledFunction();
      if (!callee || !callee->isDeclaration()) {
        continue;
      }
      if (isShadowMemHelperName(callee->getName())) {
        continue;
      }

      if (analysis.getPurity(&function) != PurityKind::Unknown ||
          analysis.getCallPurity(*call) != PurityKind::Unknown) {
        continue;
      }

      directCallers[callee->getName().str()].insert(&function);
    }
  }

  std::vector<UnknownCalleeImpact> impacts;
  impacts.reserve(directCallers.size());
  for (const auto &entry : directCallers) {
    UnknownCalleeImpact impact;
    impact.symbolName = entry.first;
    impact.directCallerCount = entry.second.size();
    const auto transitiveIt = transitiveCallers.find(impact.symbolName);
    impact.transitiveCallerCount =
        transitiveIt == transitiveCallers.end() ? 0 : transitiveIt->second.size();
    impacts.push_back(std::move(impact));
  }

  sortImpacts(impacts);
  return impacts;
}

char PurityUnknownImpactPass::ID = 0;

PurityUnknownImpactPass::PurityUnknownImpactPass() : ModulePass(ID) {}

PurityUnknownImpactPass::PurityUnknownImpactPass(
    PurityUnknownImpactPassOptions options)
    : ModulePass(ID), options_(std::move(options)) {}

bool PurityUnknownImpactPass::runOnModule(Module &module) {
  UnknownCalleeImpactAnalyzer analyzer(options_);
  const auto impacts = analyzer.rankUnknownCallees(module);

  errs() << "=== Unknown purity frontiers ===\n";
  for (const auto &impact : impacts) {
    errs() << impact.symbolName << " direct_callers=" << impact.directCallerCount
           << " transitive_callers=" << impact.transitiveCallerCount << "\n";
  }
  return false;
}

static RegisterPass<PurityUnknownImpactPass> X(
    "purity-unknown-impact",
    "Rank unknown declaration callees by reverse-call impact");

Pass *createPurityUnknownImpactPass(PurityUnknownImpactPassOptions options) {
  return new PurityUnknownImpactPass(std::move(options));
}

} // namespace lotus::analysis::purity
