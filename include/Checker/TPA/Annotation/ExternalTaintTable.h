#pragma once

#include "Checker/TPA/Annotation/TaintSummary.h"

#include <unordered_map>

#include <llvm/ADT/StringRef.h>

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace annotation {

class ExternalTaintTable {
private:
  std::unordered_map<std::string, TaintSummary> summaryMap;

public:
  using const_iterator = typename decltype(summaryMap)::const_iterator;

  ExternalTaintTable() = default;

  const TaintSummary *lookup(llvm::StringRef name) const;

  const_iterator begin() const { return summaryMap.begin(); }
  const_iterator end() const { return summaryMap.end(); }
  size_t size() const { return summaryMap.size(); }

  static bool parse(llvm::StringRef text, ExternalTaintTable &out,
                    llvm::raw_ostream &errors);

  static ExternalTaintTable loadFromFile(const char *fileName);
};

} // namespace annotation
