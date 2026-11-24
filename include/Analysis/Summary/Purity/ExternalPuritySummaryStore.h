/** @file ExternalPuritySummaryStore.h @brief Storage and retrieval of external purity summaries. */
#pragma once

#include "Analysis/Summary/Purity/DeclarationSummaryProvider.h"

#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotus {
namespace analysis {
namespace purity {

enum class ExternalSummaryState {
  Suggested = 0,
  Validated = 1,
  Rejected = 2,
};

llvm::StringRef toString(ExternalSummaryState state);

struct ExternalPuritySummaryRecord {
  std::string functionName;
  FunctionEffectSummary summary;
  ExternalSummaryState state = ExternalSummaryState::Suggested;
  std::string note;
};

class ExternalPuritySummaryStore {
public:
  bool loadFromFile(llvm::StringRef path, std::string &errorMessage);
  bool saveToFile(llvm::StringRef path, std::string &errorMessage) const;

  void upsert(ExternalPuritySummaryRecord record);
  bool contains(llvm::StringRef functionName) const;
  const ExternalPuritySummaryRecord *find(llvm::StringRef functionName) const;

  std::vector<ExternalPuritySummaryRecord> records() const;
  std::vector<ExternalPuritySummaryRecord>
  recordsForState(ExternalSummaryState state) const;

  bool updateState(llvm::StringRef functionName, ExternalSummaryState state,
                   llvm::StringRef note = {});

  std::shared_ptr<ExternalPuritySummaryProvider>
  createProvider(bool includeSuggested = false,
                 bool includeValidated = true) const;

private:
  std::unordered_map<std::string, ExternalPuritySummaryRecord> records_;
};

} // namespace purity
} // namespace analysis
} // namespace lotus
