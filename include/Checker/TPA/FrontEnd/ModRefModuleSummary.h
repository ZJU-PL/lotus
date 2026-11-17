#pragma once

#include "Checker/TPA/FrontEnd/ModRefFunctionSummary.h"

#include <cassert>
#include <unordered_map>

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace tpa::taint {

class ModRefModuleSummary {
private:
  using MapType =
      std::unordered_map<const llvm::Function *, ModRefFunctionSummary>;
  MapType summaryMap;

public:
  using const_iterator = MapType::const_iterator;

  ModRefFunctionSummary &getSummary(const llvm::Function *f) {
    return summaryMap[f];
  }
  const ModRefFunctionSummary &getSummary(const llvm::Function *f) const {
    auto itr = summaryMap.find(f);
    assert(itr != summaryMap.end());
    return itr->second;
  }

  const_iterator begin() const { return summaryMap.begin(); }
  const_iterator end() const { return summaryMap.end(); }
};

} // namespace tpa::taint