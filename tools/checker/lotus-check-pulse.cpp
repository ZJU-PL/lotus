/*
 * Pulse Checker Tool
 *
 * A bug finder using biabductive analysis, inspired by Infer's Pulse.
 * Uses UnderApproxAA for must-alias canonicalization.
 */

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/ReportOptions.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/Framework/SuppressionManager.h"
#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Domain/PulseTaintConfig.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLogger.h"
#include "Checker/Pulse/Report/PulseOptions.h"
#include "Checker/Tooling/CheckerOptions.h"
#include "Checker/Tooling/CheckerReport.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace pulse;

static cl::opt<std::string>
    InputFile(cl::Positional, cl::desc("<input bitcode>"), cl::Required,
              cl::sub(lotus::checker::tooling::pulseSubCommand()));
enum class SMTMode { Off, On };
static cl::opt<std::string> ToplProperties(
    "pulse.topl-properties", cl::desc("TOPL temporal property file"),
    cl::init(""), cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<SMTMode>
    SMT("pulse.smt", cl::desc("SMT path-feasibility solving"),
        cl::values(clEnumValN(SMTMode::Off, "off", "Disable SMT solving"),
                   clEnumValN(SMTMode::On, "on", "Enable SMT solving")),
        cl::init(SMTMode::On),
        cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<bool> ModelMallocFailure(
    "pulse.model-malloc-failure",
    cl::desc("Fork a null-return state for malloc calls"), cl::init(false),
    cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<bool> ReportLatentIssues(
    "pulse.report-latent-issues",
    cl::desc("Report latent issues even without a manifesting caller"),
    cl::init(false), cl::sub(lotus::checker::tooling::pulseSubCommand()));
static cl::opt<std::string> TaintConfigPath(
    "pulse.taint-config",
    cl::desc("JSON file specifying custom Pulse taint sources/sinks/policies"),
    cl::init(""), cl::sub(lotus::checker::tooling::pulseSubCommand()));

int runPulseCheckerTool(const char *argv0) {
  (void)lotus::checker::tooling::statsEnabled();
  auto selectedOr =
      lotus::checker::tooling::resolveChecks(lotus::checker::EngineKind::Pulse);
  if (!selectedOr) {
    logAllUnhandledErrors(selectedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  const auto &selected = *selectedOr;
  std::optional<ToplProgram> topl;
  if (!ToplProperties.empty()) {
    auto buffer = MemoryBuffer::getFile(ToplProperties);
    if (!buffer) {
      errs() << "error: cannot read TOPL properties: " << buffer.getError().message() << '\n';
      return lotus::checker::tooling::EXIT_ERROR;
    }
    auto parsed = ToplProgram::parse((*buffer)->getBuffer(), ToplProperties);
    if (!parsed) {
      logAllUnhandledErrors(parsed.takeError(), errs(), "error: ");
      return lotus::checker::tooling::EXIT_ERROR;
    }
    topl = std::move(*parsed);
  } else if (lotus::checker::tooling::hasExplicitCheckSelection() &&
             lotus::checker::tooling::Checks != "all" && selected.count("topl")) {
    errs() << "error: TOPL requires --pulse.topl-properties\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Configure logging
  pulse::LogLevel level =
      static_cast<pulse::LogLevel>(lotus::checker::tooling::logLevel());

  PulseLogger::setLevel(level);
  PulseLogger::setOutputStream(&errs());
  PulseLogger::resetStats();

  std::optional<TaintConfig> customTaintConfig;
  if (!TaintConfigPath.empty()) {
    auto buffer = MemoryBuffer::getFile(TaintConfigPath);
    if (!buffer) {
      errs() << "error: cannot read taint config: "
             << buffer.getError().message() << '\n';
      return lotus::checker::tooling::EXIT_ERROR;
    }
    auto parsed = TaintConfig::loadFromJson((*buffer)->getBuffer(), true);
    if (!parsed) {
      logAllUnhandledErrors(parsed.takeError(), errs(), "error: ");
      return lotus::checker::tooling::EXIT_ERROR;
    }
    customTaintConfig = std::move(*parsed);
    TaintOperations::setConfig(&*customTaintConfig);
  }

  const bool disableSMT = SMT == SMTMode::Off;
  pulse::options::setDisableSMT(disableSMT);
  pulse::options::setModelMallocFailure(ModelMallocFailure);
  pulse::options::setReportLatentIssues(ReportLatentIssues);
  if (disableSMT) {
    PulseLogger::info("Fast mode: SMT solving disabled");
  }

  SMDiagnostic Err;
  LLVMContext Context;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Context);

  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("pulse", *M, mgr);

  PulseLogger::info("Starting Pulse analysis");
  PulseLogger::info("Module: " + M->getName().str());
  PulseLogger::startTimer("total_analysis");

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *M, lotus::AAConfig::UnderApprox());
  if (!AA->isInitialized()) {
    errs() << "error: alias analysis failed to initialize\n";
    PulseLogger::endTimer("total_analysis");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  PulseChecker checker(M.get(), AA.get());
  if (topl && selected.count("topl"))
    checker.setToplProgram(std::move(*topl));

  PulseLogger::startTimer("analysis");
  checker.analyze();
  PulseLogger::endTimer("analysis");

  PulseLogger::endTimer("total_analysis");

  const std::pair<StringRef, StringRef> checkBugTypes[] = {
      {"topl", "TOPL Error"},
      {"null-deref", IssueType::NullDereference},
      {"use-after-free", IssueType::UseAfterFree},
      {"out-of-bounds", IssueType::OutOfBounds},
      {"invalid-free", IssueType::InvalidFree},
      {"uninitialized-read", IssueType::UninitializedRead},
      {"taint-flow", IssueType::TaintError},
      {"unnecessary-copy", IssueType::UnnecessaryCopy},
      {"stack-address-escape", IssueType::StackVariableAddressEscape},
      {"const-refable-parameter", IssueType::ConstRefableParam},
      {"memory-leak", IssueType::MemoryLeak},
      {"resource-leak", IssueType::ResourceLeak},
      {"mutual-recursion-cycle", IssueType::MutualRecursionCycle},
  };
  std::set<std::string> disabledBugTypes;
  for (const auto &[id, bugType] : checkBugTypes) {
    if (!selected.count(id.str())) {
      disabledBugTypes.insert(bugType.str());
    }
  }
  std::vector<int> disabledTypeIds;
  for (size_t id = 0; id < mgr.get_num_bug_types(); ++id) {
    if (disabledBugTypes.count(
            mgr.get_bug_type_info(static_cast<int>(id)).bug_name)) {
      disabledTypeIds.push_back(static_cast<int>(id));
    }
  }
  mgr.clear_reports_for_types(disabledTypeIds);
  stats.emit();

  lotus::checker::tooling::CheckerReportOptions reportOptions;
  reportOptions.verbose = lotus::checker::tooling::Verbose;
  const int reportStatus =
      lotus::checker::tooling::emitCheckerReports(mgr, reportOptions);
  PulseLogger::info("Analysis complete");
  return reportStatus;
}
