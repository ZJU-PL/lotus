#include "Analysis/Summary/Purity/ExternalPuritySummaryStore.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <system_error>
#include <utility>

namespace lotus::analysis::purity {

using namespace llvm;

namespace {

std::optional<ExternalSummaryState> parseState(StringRef value) {
  if (value == "suggested") {
    return ExternalSummaryState::Suggested;
  }
  if (value == "validated") {
    return ExternalSummaryState::Validated;
  }
  if (value == "rejected") {
    return ExternalSummaryState::Rejected;
  }
  return std::nullopt;
}

std::optional<PurityKind> parsePurityKind(StringRef value) {
  if (value == "const") {
    return PurityKind::Const;
  }
  if (value == "pure") {
    return PurityKind::Pure;
  }
  if (value == "impure") {
    return PurityKind::Impure;
  }
  if (value == "unknown") {
    return PurityKind::Unknown;
  }
  return std::nullopt;
}

std::optional<SummarySource> parseSummarySource(StringRef value) {
  if (value == "internal-analysis") {
    return SummarySource::InternalAnalysis;
  }
  if (value == "local-attributes") {
    return SummarySource::LocalAttributes;
  }
  if (value == "builtin-spec") {
    return SummarySource::BuiltinSpec;
  }
  if (value == "memoryssa") {
    return SummarySource::MemorySSA;
  }
  if (value == "external-summary") {
    return SummarySource::ExternalSummary;
  }
  if (value == "propagated") {
    return SummarySource::Propagated;
  }
  if (value == "conservative-fallback") {
    return SummarySource::ConservativeFallback;
  }
  return std::nullopt;
}

std::optional<SummaryConfidence> parseSummaryConfidence(StringRef value) {
  if (value == "low") {
    return SummaryConfidence::Low;
  }
  if (value == "medium") {
    return SummaryConfidence::Medium;
  }
  if (value == "high") {
    return SummaryConfidence::High;
  }
  return std::nullopt;
}

json::Array toJsonDependencies(const FunctionEffectSummary &summary) {
  json::Array dependencies;
  for (const std::string &dependency : summary.dependsOn) {
    dependencies.push_back(dependency);
  }
  return dependencies;
}

json::Object toJsonSummary(const FunctionEffectSummary &summary) {
  return json::Object{
      {"purity", toString(summary.getPurityKind()).str()},
      {"reads_reachable_memory", summary.readsReachableMemory},
      {"writes_reachable_memory", summary.writesReachableMemory},
      {"has_observable_side_effects", summary.hasObservableSideEffects},
      {"has_unknown_effects", summary.hasUnknownEffects},
      {"from_memoryssa", summary.fromMemorySSA},
      {"source", toString(summary.source).str()},
      {"confidence", toString(summary.confidence).str()},
      {"depends_on", toJsonDependencies(summary)},
  };
}

FunctionEffectSummary summaryFromPurityKind(PurityKind kind) {
  FunctionEffectSummary summary;
  switch (kind) {
  case PurityKind::Const:
    return summary;
  case PurityKind::Pure:
    summary.readsReachableMemory = true;
    return summary;
  case PurityKind::Impure:
    summary.writesReachableMemory = true;
    return summary;
  case PurityKind::Unknown:
    summary.hasUnknownEffects = true;
    return summary;
  }
  return summary;
}

bool parseSummaryObject(const json::Object &object,
                        FunctionEffectSummary &summary,
                        std::string &errorMessage) {
  if (Optional<StringRef> purityText = object.getString("purity")) {
    const auto purityKind = parsePurityKind(*purityText);
    if (!purityKind) {
      errorMessage = "unknown purity kind: " + purityText->str();
      return false;
    }
    summary = summaryFromPurityKind(*purityKind);
  }

  if (Optional<bool> readsMemory = object.getBoolean("reads_reachable_memory")) {
    summary.readsReachableMemory = *readsMemory;
  }
  if (Optional<bool> writesMemory =
          object.getBoolean("writes_reachable_memory")) {
    summary.writesReachableMemory = *writesMemory;
  }
  if (Optional<bool> sideEffects =
          object.getBoolean("has_observable_side_effects")) {
    summary.hasObservableSideEffects = *sideEffects;
  }
  if (Optional<bool> unknownEffects =
          object.getBoolean("has_unknown_effects")) {
    summary.hasUnknownEffects = *unknownEffects;
  }
  if (Optional<bool> fromMemorySSA = object.getBoolean("from_memoryssa")) {
    summary.fromMemorySSA = *fromMemorySSA;
  }

  if (Optional<StringRef> sourceText = object.getString("source")) {
    const auto source = parseSummarySource(*sourceText);
    if (!source) {
      errorMessage = "unknown summary source: " + sourceText->str();
      return false;
    }
    summary.source = *source;
  }

  if (Optional<StringRef> confidenceText = object.getString("confidence")) {
    const auto confidence = parseSummaryConfidence(*confidenceText);
    if (!confidence) {
      errorMessage = "unknown summary confidence: " + confidenceText->str();
      return false;
    }
    summary.confidence = *confidence;
  }

  summary.dependsOn.clear();
  if (const auto *dependencies = object.getArray("depends_on")) {
    for (const json::Value &entry : *dependencies) {
      Optional<StringRef> dependency = entry.getAsString();
      if (!dependency) {
        errorMessage = "depends_on entries must be strings";
        return false;
      }
      summary.addDependency(*dependency);
    }
  }

  return true;
}

json::Object toJsonRecord(const ExternalPuritySummaryRecord &record) {
  return json::Object{
      {"function", record.functionName},
      {"state", toString(record.state).str()},
      {"note", record.note},
      {"summary", toJsonSummary(record.summary)},
  };
}

} // namespace

StringRef toString(ExternalSummaryState state) {
  switch (state) {
  case ExternalSummaryState::Suggested:
    return "suggested";
  case ExternalSummaryState::Validated:
    return "validated";
  case ExternalSummaryState::Rejected:
    return "rejected";
  }
  llvm_unreachable("unknown ExternalSummaryState");
}

bool ExternalPuritySummaryStore::loadFromFile(StringRef path,
                                              std::string &errorMessage) {
  records_.clear();

  auto bufferOrErr = MemoryBuffer::getFile(path);
  if (!bufferOrErr) {
    errorMessage = bufferOrErr.getError().message();
    return false;
  }

  Expected<json::Value> parsed = json::parse(bufferOrErr.get()->getBuffer());
  if (!parsed) {
    errorMessage = toString(parsed.takeError());
    return false;
  }

  const auto *root = parsed->getAsObject();
  if (!root) {
    errorMessage = "expected JSON object at file root";
    return false;
  }

  const auto *summaries = root->getArray("summaries");
  if (!summaries) {
    return true;
  }

  for (const json::Value &entry : *summaries) {
    const auto *object = entry.getAsObject();
    if (!object) {
      errorMessage = "summary entries must be JSON objects";
      return false;
    }

    Optional<StringRef> functionName = object->getString("function");
    if (!functionName || functionName->empty()) {
      errorMessage = "summary entry missing function name";
      return false;
    }

    ExternalPuritySummaryRecord record;
    record.functionName = functionName->str();
    if (Optional<StringRef> note = object->getString("note")) {
      record.note = note->str();
    }

    if (Optional<StringRef> stateText = object->getString("state")) {
      const auto state = parseState(*stateText);
      if (!state) {
        errorMessage = "unknown external summary state: " + stateText->str();
        return false;
      }
      record.state = *state;
    }

    const auto *summaryObject = object->getObject("summary");
    if (!summaryObject) {
      errorMessage = "summary entry missing summary object";
      return false;
    }

    if (!parseSummaryObject(*summaryObject, record.summary, errorMessage)) {
      return false;
    }

    records_[record.functionName] = std::move(record);
  }

  return true;
}

bool ExternalPuritySummaryStore::saveToFile(StringRef path,
                                            std::string &errorMessage) const {
  std::error_code ec;
  raw_fd_ostream os(path, ec, sys::fs::OF_Text);
  if (ec) {
    errorMessage = ec.message();
    return false;
  }

  json::Array summaries;
  for (const auto &record : records()) {
    summaries.push_back(toJsonRecord(record));
  }

  json::Object root;
  root["version"] = 1;
  root["summaries"] = std::move(summaries);

  os << formatv("{0:2}\n", json::Value(std::move(root)));
  return true;
}

void ExternalPuritySummaryStore::upsert(ExternalPuritySummaryRecord record) {
  records_[record.functionName] = std::move(record);
}

bool ExternalPuritySummaryStore::contains(StringRef functionName) const {
  return records_.find(functionName.str()) != records_.end();
}

const ExternalPuritySummaryRecord *
ExternalPuritySummaryStore::find(StringRef functionName) const {
  auto it = records_.find(functionName.str());
  return it == records_.end() ? nullptr : &it->second;
}

std::vector<ExternalPuritySummaryRecord>
ExternalPuritySummaryStore::records() const {
  std::vector<ExternalPuritySummaryRecord> result;
  result.reserve(records_.size());
  for (const auto &entry : records_) {
    result.push_back(entry.second);
  }
  llvm::sort(result, [](const ExternalPuritySummaryRecord &lhs,
                        const ExternalPuritySummaryRecord &rhs) {
    return lhs.functionName < rhs.functionName;
  });
  return result;
}

std::vector<ExternalPuritySummaryRecord>
ExternalPuritySummaryStore::recordsForState(ExternalSummaryState state) const {
  std::vector<ExternalPuritySummaryRecord> result;
  for (const auto &record : records()) {
    if (record.state == state) {
      result.push_back(record);
    }
  }
  return result;
}

bool ExternalPuritySummaryStore::updateState(StringRef functionName,
                                             ExternalSummaryState state,
                                             StringRef note) {
  auto it = records_.find(functionName.str());
  if (it == records_.end()) {
    return false;
  }

  it->second.state = state;
  if (!note.empty()) {
    it->second.note = note.str();
  }
  return true;
}

std::shared_ptr<ExternalPuritySummaryProvider>
ExternalPuritySummaryStore::createProvider(bool includeSuggested,
                                           bool includeValidated) const {
  auto provider = std::make_shared<ExternalPuritySummaryProvider>();
  for (const auto &record : records()) {
    if (record.state == ExternalSummaryState::Rejected) {
      continue;
    }
    if (record.state == ExternalSummaryState::Suggested && !includeSuggested) {
      continue;
    }
    if (record.state == ExternalSummaryState::Validated && !includeValidated) {
      continue;
    }
    provider->setSummary(record.functionName, record.summary);
  }
  return provider;
}

} // namespace lotus::analysis::purity
