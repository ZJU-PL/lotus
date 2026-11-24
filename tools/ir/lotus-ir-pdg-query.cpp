#include "IR/PDG/Construction/GraphBuilder.h"
/**
 * @file lotus-ir-pdg-query.cpp
 * @brief Initialize LLVM, load IR, build PDG, and launch the query driver.
 */

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/PDG/Construction/ControlDependencyGraph.h"
#include "IR/PDG/Construction/DataDependencyGraph.h"
#include "IR/PDG/Construction/ProgramDependencyGraph.h"
#include "pdg-query/Driver.h"
#include "pdg-query/Options.h"

#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

int main(int argc, char **argv) {
  llvm::InitLLVM init(argc, argv);
  const auto config = lotus::pdg_query::parseOptions(argc, argv);

  // Alias-analysis diagnostics must not corrupt machine-readable results.
  if (config.format == "json")
    spdlog::set_default_logger(spdlog::stderr_logger_mt("pdg-query"));

  if (auto status = lotus::pdg_query::handleCatalogOptions(config))
    return *status;

  llvm::LLVMContext context;
  llvm::SMDiagnostic error;
  auto module = llvm::parseIRFile(config.inputFilename, error, context);
  if (!module) {
    error.print(argv[0], llvm::errs());
    return 1;
  }

  auto &pdg = pdg::ProgramGraph::getInstance();
  if (config.buildPDG) {
    auto &registry = *llvm::PassRegistry::getPassRegistry();
    llvm::initializeCore(registry);
    llvm::initializeAnalysis(registry);
    llvm::initializeTransformUtils(registry);
    llvm::legacy::PassManager passes;
    passes.add(new pdg::DataDependencyGraph());
    passes.add(new pdg::ControlDependencyGraph());
    passes.add(new pdg::ProgramDependencyGraph());
    passes.run(*module);
  } else {
    pdg.reset();
    pdg::GraphBuilder(pdg).build(*module);
    pdg::GraphBuilder(pdg).bindDebugInfo(*module);
  }

  return lotus::pdg_query::runQueries(pdg, *module, config);
}
