#include "Checker/Framework/Subcommands.h"
#include "Checker/GSAF/API/VulnerabilityRegistry.h"
#include "Checker/GSAF/Engine/Checker.h"
#include "Checker/Tooling/CheckerOptions.h"
#include "Checker/Tooling/CheckerReport.h"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;
using namespace lotus::gsaf;
namespace tooling = lotus::checker::tooling;
static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required,
                                          cl::sub(tooling::gsafSubCommand()));

int runGSAFCheckerTool(const char *argv0) {
  auto selected = tooling::resolveChecks(lotus::checker::EngineKind::GSAF);
  if (!selected) {
    logAllUnhandledErrors(selected.takeError(), errs(), "error: ");
    return tooling::EXIT_ERROR;
  }
  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseIRFile(InputFilename, diagnostic, context);
  if (!module) {
    diagnostic.print(argv0, errs());
    return tooling::EXIT_ERROR;
  }
  auto &registry = *PassRegistry::getPassRegistry();
  initializeCore(registry);
  initializeAnalysis(registry);
  initializeTransformUtils(registry);
  auto &manager = BugReportMgr::get_instance();
  tooling::AnalysisStatsRecorder stats("gsaf", *module, manager);
  legacy::PassManager passes;
  std::vector<std::shared_ptr<VulnerabilityWrapper>> taint;
  for (auto *info : registeredVulnerabilities()) {
    // Keep the option's owning string alive while extracting the checker name.
    std::string option = info->id();
    StringRef name(option);
    name.consume_front("gsaf.");
    if (!selected->count(name.str()) && !info->enabled())
      continue;
    auto vulnerability = info->getVulnerability();
    auto wrapper = std::make_shared<VulnerabilityWrapper>();
    wrapper->addVulnerability(vulnerability);
    taint.push_back(std::move(wrapper));
  }
  std::map<std::shared_ptr<Vulnerability>, GSAFChecker *> compositeCheckers;
  for (auto *info : registeredMultiVulnerabilities()) {
    if (!info->enabled())
      continue;
    for (const auto &part : *info->Checker) {
      part->setParasitical(true);
      auto wrapper = std::make_shared<VulnerabilityWrapper>();
      wrapper->addVulnerability(part);
      auto *checker = new GSAFChecker(wrapper);
      compositeCheckers[part] = checker;
      passes.add(checker);
    }
  }
  for (auto &wrapper : taint)
    passes.add(new GSAFChecker(wrapper));
  passes.run(*module);
  for (auto *info : registeredMultiVulnerabilities())
    if (info->enabled())
      buildMultiVulnerability(info->Checker, compositeCheckers);
  stats.emit();
  return tooling::emitCheckerReports(manager, {tooling::Verbose});
}
