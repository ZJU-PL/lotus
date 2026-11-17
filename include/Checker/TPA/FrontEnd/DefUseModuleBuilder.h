#pragma once

#include "Annotation/ModRef/ExternalModRefTable.h"
#include "Checker/TPA/Program/DefUseModule.h"

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
  annotation::ExternalModRefTable modRefTable;

  void buildDefUseFunction(DefUseFunction &f, const ModRefModuleSummary &);
  void buildMemLevelEdges(DefUseFunction &f, const ModRefModuleSummary &);

public:
  DefUseModuleBuilder(const tpa::SemiSparsePointerAnalysis &p)
      : ptrAnalysis(p) {}

  DefUseModule buildDefUseModule(const llvm::Module &module);

  void loadExternalModRefTable(const char *fileName) {
    modRefTable = annotation::ExternalModRefTable::loadFromFile(fileName);
  }
};

} // namespace tpa::taint
