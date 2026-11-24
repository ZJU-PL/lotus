#include "Analysis/Summary/Purity/PurityInferenceDriver.h"

#include "Analysis/Summary/Purity/FunctionPurityAnalysis.h"
#include "Analysis/Summary/Purity/PurityAttrInferencePass.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <utility>

namespace lotus::analysis::purity {

using namespace llvm;

namespace {

json::Array toJsonDependencies(const std::vector<std::string> &dependsOn) {
  json::Array entries;
  for (const std::string &dependency : dependsOn) {
    entries.push_back(dependency);
  }
  return entries;
}

json::Object toJsonFunctionReport(const PurityFunctionReport &function) {
  return json::Object{
      {"function", function.functionName},
      {"purity", toString(function.purity).str()},
      {"source", toString(function.source).str()},
      {"confidence", toString(function.confidence).str()},
      {"depends_on", toJsonDependencies(function.dependsOn)},
  };
}

json::Object toJsonUnknownSummaryReport(
    const PurityUnknownSummaryReport &report) {
  return json::Object{
      {"function", report.impact.symbolName},
      {"direct_callers", static_cast<int64_t>(report.impact.directCallerCount)},
      {"transitive_callers",
       static_cast<int64_t>(report.impact.transitiveCallerCount)},
      {"has_stored_summary", report.hasStoredSummary},
      {"stored_state",
       report.hasStoredSummary ? toString(report.storedState).str() : ""},
  };
}

} // namespace

std::vector<std::string>
PurityInvalidationAnalyzer::collectImpactedFunctions(
    Module &module, const FunctionPurityAnalysis &analysis,
    ArrayRef<std::string> invalidatedSummaries) {
  std::set<std::string> invalidated(invalidatedSummaries.begin(),
                                    invalidatedSummaries.end());
  std::vector<std::string> impacted;

  for (Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }

    const auto effects = analysis.getEffects(&function);
    for (const std::string &dependency : effects.dependsOn) {
      if (invalidated.count(dependency) != 0U) {
        impacted.push_back(function.getName().str());
        break;
      }
    }
  }

  llvm::sort(impacted);
  impacted.erase(std::unique(impacted.begin(), impacted.end()), impacted.end());
  return impacted;
}

PurityInferenceDriver::PurityInferenceDriver(PurityInferenceDriverOptions options)
    : options_(std::move(options)) {}

PurityInferenceReport
PurityInferenceDriver::run(Module &module,
                           ExternalPuritySummaryStore &summaryStore) const {
  PurityInferenceReport report;

  FunctionPurityAnalysisOptions preInvalidationOptions;
  preInvalidationOptions.externalSummaryProviders.push_back(
      summaryStore.createProvider(options_.includeSuggestedSummaries, true));

  if (!options_.invalidatedSummaries.empty()) {
    FunctionPurityAnalysis preInvalidationAnalysis(module,
                                                   preInvalidationOptions);
    preInvalidationAnalysis.run();
    report.invalidatedFunctions = PurityInvalidationAnalyzer::collectImpactedFunctions(
        module, preInvalidationAnalysis, options_.invalidatedSummaries);

    for (const std::string &summaryName : options_.invalidatedSummaries) {
      summaryStore.updateState(summaryName, ExternalSummaryState::Rejected,
                               "invalidated by purity driver");
    }
  }

  auto provider = summaryStore.createProvider(options_.includeSuggestedSummaries,
                                              true);

  FunctionPurityAnalysisOptions analysisOptions;
  analysisOptions.externalSummaryProviders.push_back(provider);
  FunctionPurityAnalysis analysis(module, analysisOptions);
  analysis.run();

  if (options_.applyAttributes) {
    report.attributesApplied = inferPurityAttributes(module, analysis);
  }

  UnknownCalleeImpactAnalyzer impactAnalyzer(
      PurityUnknownImpactPassOptions{{provider}});
  const auto impacts = impactAnalyzer.rankUnknownCallees(module);
  report.unknownSummaries.reserve(impacts.size());
  for (const auto &impact : impacts) {
    PurityUnknownSummaryReport entry;
    entry.impact = impact;
    if (const auto *record = summaryStore.find(impact.symbolName)) {
      entry.hasStoredSummary = true;
      entry.storedState = record->state;
    }
    report.unknownSummaries.push_back(std::move(entry));
  }

  for (Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }

    const auto effects = analysis.getEffects(&function);
    PurityFunctionReport functionReport;
    functionReport.functionName = function.getName().str();
    functionReport.purity = effects.getPurityKind();
    functionReport.source = effects.source;
    functionReport.confidence = effects.confidence;
    functionReport.dependsOn = effects.dependsOn;
    report.functions.push_back(std::move(functionReport));
  }

  llvm::sort(report.functions, [](const PurityFunctionReport &lhs,
                                  const PurityFunctionReport &rhs) {
    return lhs.functionName < rhs.functionName;
  });

  return report;
}

void printPurityInferenceReport(const PurityInferenceReport &report,
                                raw_ostream &os) {
  os << "== Unknown summaries ==\n";
  for (const auto &unknown : report.unknownSummaries) {
    os << unknown.impact.symbolName
       << " direct_callers=" << unknown.impact.directCallerCount
       << " transitive_callers=" << unknown.impact.transitiveCallerCount;
    if (unknown.hasStoredSummary) {
      os << " stored_state=" << toString(unknown.storedState);
    }
    os << "\n";
  }

  os << "== Function purity ==\n";
  for (const auto &function : report.functions) {
    os << function.functionName << " purity=" << toString(function.purity)
       << " source=" << toString(function.source)
       << " confidence=" << toString(function.confidence);
    if (!function.dependsOn.empty()) {
      os << " depends_on=";
      for (size_t index = 0; index < function.dependsOn.size(); ++index) {
        if (index != 0U) {
          os << ",";
        }
        os << function.dependsOn[index];
      }
    }
    os << "\n";
  }

  if (!report.invalidatedFunctions.empty()) {
    os << "== Invalidated functions ==\n";
    for (const std::string &functionName : report.invalidatedFunctions) {
      os << functionName << "\n";
    }
  }
}

std::string renderPurityInferenceReportAsJson(
    const PurityInferenceReport &report) {
  json::Array unknownSummaries;
  for (const auto &entry : report.unknownSummaries) {
    unknownSummaries.push_back(toJsonUnknownSummaryReport(entry));
  }

  json::Array functions;
  for (const auto &entry : report.functions) {
    functions.push_back(toJsonFunctionReport(entry));
  }

  json::Array invalidatedFunctions;
  for (const std::string &functionName : report.invalidatedFunctions) {
    invalidatedFunctions.push_back(functionName);
  }

  json::Object root{
      {"unknown_summaries", std::move(unknownSummaries)},
      {"functions", std::move(functions)},
      {"invalidated_functions", std::move(invalidatedFunctions)},
      {"attributes_applied", report.attributesApplied},
  };

  return formatv("{0:2}", json::Value(std::move(root))).str();
}

} // namespace lotus::analysis::purity
