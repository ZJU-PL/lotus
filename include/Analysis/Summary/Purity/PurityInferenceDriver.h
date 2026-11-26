/** @file PurityInferenceDriver.h @brief Driver for purity inference across compilation units. */
#pragma once

#include "Analysis/Summary/Purity/FunctionPurityAnalysis.h"
#include "Analysis/Summary/Purity/ExternalPuritySummaryStore.h"
#include "Analysis/Summary/Purity/PurityUnknownImpactPass.h"

#include "llvm/ADT/ArrayRef.h"

#include <string>
#include <vector>

namespace llvm {
class Module;
class raw_ostream;
} // namespace llvm

namespace lotus {
namespace analysis {
namespace purity {

struct PurityFunctionReport {
  std::string functionName;
  PurityKind purity = PurityKind::Unknown;
  SummarySource source = SummarySource::ConservativeFallback;
  SummaryConfidence confidence = SummaryConfidence::Low;
  std::vector<std::string> dependsOn;
};

struct PurityUnknownSummaryReport {
  UnknownCalleeImpact impact;
  bool hasStoredSummary = false;
  ExternalSummaryState storedState = ExternalSummaryState::Suggested;
};

struct PurityInferenceReport {
  std::vector<PurityUnknownSummaryReport> unknownSummaries;
  std::vector<PurityFunctionReport> functions;
  std::vector<std::string> invalidatedFunctions;
  bool attributesApplied = false;
};

struct PurityInferenceDriverOptions {
  bool includeSuggestedSummaries = false;
  bool applyAttributes = false;
  std::vector<std::string> invalidatedSummaries;
};

class PurityInvalidationAnalyzer {
public:
  static std::vector<std::string>
  collectImpactedFunctions(llvm::Module &module,
                           const FunctionPurityAnalysis &analysis,
                           llvm::ArrayRef<std::string> invalidatedSummaries);
};

class PurityInferenceDriver {
public:
  explicit PurityInferenceDriver(PurityInferenceDriverOptions options = {});

  PurityInferenceReport run(llvm::Module &module,
                            ExternalPuritySummaryStore &summaryStore) const;

private:
  PurityInferenceDriverOptions options_;
};

void printPurityInferenceReport(const PurityInferenceReport &report,
                                llvm::raw_ostream &os);
std::string renderPurityInferenceReportAsJson(
    const PurityInferenceReport &report);

} // namespace purity
} // namespace analysis
} // namespace lotus
