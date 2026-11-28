#pragma once

#include "Annotation/ModRef/ExternalModRefTable.h"
#include "Checker/TPA/Program/DefUseModule.h"
#include "Checker/TPA/Support/ExternalModels.h"

#include <utility>

namespace llvm {
class Module;
class Value;
} // namespace llvm

namespace tpa {
class SemiSparsePointerAnalysis;
} // namespace tpa

namespace tpa::taint {

class ModRefModuleSummary;

class DefUseModuleBuilder {
private:
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;
  UnmodeledPolicy unmodeledPolicy;
  annotation::ExternalModRefTable modRefTable;
  bool enableMemoryGrouping = true;

  void buildDefUseFunction(DefUseFunction &f, const ModRefModuleSummary &);
  void buildMemLevelEdges(DefUseFunction &f, const ModRefModuleSummary &);

public:
  DefUseModuleBuilder(const tpa::SemiSparsePointerAnalysis &p,
                      UnmodeledPolicy policy = UnmodeledPolicy::Ignore)
      : ptrAnalysis(p), unmodeledPolicy(policy) {}

  void setEnableMemoryGrouping(bool enable) { enableMemoryGrouping = enable; }
  bool isMemoryGroupingEnabled() const { return enableMemoryGrouping; }

  DefUseModule buildDefUseModule(const llvm::Module &module);

  void setExternalModRefTable(annotation::ExternalModRefTable table) {
    modRefTable = std::move(table);
  }

  void loadExternalModRefTable(const char *fileName) {
    modRefTable = annotation::ExternalModRefTable::loadFromFile(fileName);
  }
};

} // namespace tpa::taint
