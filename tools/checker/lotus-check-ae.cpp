//===- lotus-check-ae.cpp -- Abstract Execution Bug Checker --------//
//
// Lotus tool for buffer overflow and null pointer dereference detection
// using Abstract Execution. Migrated from SVF's AE engine.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/AEDetector.h"
#include "Checker/AE/AbstractInterpretation.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/ReportOptions.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/Framework/SuppressionManager.h"
#include "Checker/Tooling/CheckerOptions.h"
#include "Checker/Tooling/CheckerReport.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                  cl::Required,
                  cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<lotus::analysis::AbstractInterpretation::HandleRecur>
    HandleRecurOpt(
        "ae.recursion-mode", cl::desc("Recursion handling mode"),
        cl::values(
            clEnumValN(lotus::analysis::AbstractInterpretation::TOP, "top",
                       "Set recursive stores/returns to TOP"),
            clEnumValN(lotus::analysis::AbstractInterpretation::WIDEN_ONLY,
                       "widen-only", "Widening only on recursion"),
            clEnumValN(lotus::analysis::AbstractInterpretation::WIDEN_NARROW,
                       "widen-narrow", "Widening + narrowing on recursion")),
        cl::init(lotus::analysis::AbstractInterpretation::WIDEN_NARROW),
        cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<unsigned>
    WidenDelayOpt("ae.widen-delay-iterations",
                  cl::desc("Delay widening for this many loop iterations"),
                  cl::init(3),
                  cl::sub(lotus::checker::tooling::aeSubCommand()));

enum class CheckpointPolicy { Permissive, Strict };
static cl::opt<CheckpointPolicy> CheckpointPolicyOpt(
    "ae.checkpoint-policy", cl::desc("Unchecked-checkpoint handling policy"),
    cl::values(clEnumValN(CheckpointPolicy::Permissive, "permissive",
                          "Allow unchecked checkpoints"),
               clEnumValN(CheckpointPolicy::Strict, "strict",
                          "Fail when checkpoints remain unchecked")),
    cl::init(CheckpointPolicy::Strict),
    cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<bool> StateStoreStatsOpt(
    "ae.state-store-stats",
    cl::desc("Report how many abstract states each instruction retained"),
    cl::init(false), cl::sub(lotus::checker::tooling::aeSubCommand()));

enum class FunEntryMode { Main, NoMain };
static cl::opt<FunEntryMode> FunEntryOpt(
    "ae.fun-entry", cl::desc("Where abstract execution starts (default: main)"),
    cl::values(clEnumValN(FunEntryMode::Main, "main",
                          "Analyze entry points reachable from main"),
               clEnumValN(FunEntryMode::NoMain, "no-main",
                          "Analyze each call-graph root SCC")),
    cl::init(FunEntryMode::Main),
    cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<int64_t> TimeoutOpt(
    "ae.timeout",
    cl::desc("Wall-clock budget for the whole analysis in seconds "
             "(0 disables; findings so far are still reported)"),
    cl::init(0), cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<std::string> SparsityOpt(
    "ae.sparsity",
    cl::desc("SVF-compatible spelling of --ae.state-store: "
             "dense|semi-sparse|sparse"),
    cl::value_desc("dense|semi-sparse|sparse"), cl::init(""),
    cl::sub(lotus::checker::tooling::aeSubCommand()));

static cl::opt<std::string> StateLayoutOpt(
    "ae.state-store",
    cl::desc("Per-instruction abstract state retention (default: dense). "
             "'sparse' drops states no later reader can observe; findings are "
             "identical, peak memory is lower"),
    cl::value_desc("dense|sparse|semi-sparse"),
    cl::init("dense"), cl::sub(lotus::checker::tooling::aeSubCommand()));
int runAECheckerTool(const char *argv0) {
  auto selectedOr =
      lotus::checker::tooling::resolveChecks(lotus::checker::EngineKind::AE);
  if (!selectedOr) {
    logAllUnhandledErrors(selectedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  const auto &selected = *selectedOr;

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  if (FunEntryOpt == FunEntryMode::Main) {
    const Function *entry = M->getFunction("main");
    if (!entry || entry->isDeclaration())
      entry = M->getFunction("svf.main");
    if (!entry || entry->isDeclaration()) {
      errs() << "AE requires a defined main/svf.main entry; use "
                "--ae.fun-entry=no-main for a library module\n";
      return lotus::checker::tooling::EXIT_ERROR;
    }
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("ae", *M, mgr);

  const bool runOverflow = selected.count("buffer-overflow");
  const bool runNullDeref = selected.count("null-deref");
  const bool runUseAfterFree = selected.count("use-after-free");
  const bool runInvalidFree = selected.count("invalid-free");
  const bool runMemLeak = selected.count("memory-leak");

  // Run AE analysis
  lotus::analysis::AbstractInterpretation &ae =
      lotus::analysis::AbstractInterpretation::getAEInstance();
  // The checker frontend can be invoked repeatedly in one process. Start each
  // invocation from a clean configuration instead of accumulating detectors
  // and options in the AE singleton.
  ae.reset();
  ae.setRecursionMode(HandleRecurOpt);
  ae.setWidenDelay(WidenDelayOpt);
  ae.setStrictCheckpoint(CheckpointPolicyOpt == CheckpointPolicy::Strict);
  ae.setPrintStats(StateStoreStatsOpt ||
                   lotus::checker::tooling::statsEnabled());
  ae.setFunEntryMode(FunEntryOpt == FunEntryMode::NoMain
                         ? lotus::analysis::AbstractInterpretation::FunEntryMode::NoMain
                         : lotus::analysis::AbstractInterpretation::FunEntryMode::Main);
  if (TimeoutOpt < 0) {
    errs() << "--ae.timeout must be >= 0 (0 disables the budget)\n";
    return 2;
  }
  ae.setTimeLimitSeconds(TimeoutOpt);
  {
    // `--ae.sparsity` mirrors SVF's flag name; `--ae.state-store` is the
    // Lotus-native spelling. Whichever was passed explicitly wins; with
    // neither, fall back to the documented default (`dense`).
    std::string requested = StateLayoutOpt;
    if (StateLayoutOpt.getNumOccurrences() == 0 &&
        SparsityOpt.getNumOccurrences() != 0)
      requested = SparsityOpt.getValue();
    lotus::analysis::StateLayout layout = lotus::analysis::StateLayout::Dense;
    if (!lotus::analysis::parseStateLayout(requested, layout)) {
      errs() << "unknown AE sparsity value '" << requested
             << "' (expected dense, semi-sparse or sparse)\n";
      return 2;
    }
    ae.setStateLayout(layout);
  }
  ae.setEnableBufOverflowCheck(runOverflow);
  ae.setEnableNullDerefCheck(runNullDeref);
  ae.setEnableMemLeakCheck(runMemLeak);

  // Add detectors based on options
  if (runOverflow) {
    ae.addDetector(std::make_unique<lotus::analysis::BufOverflowDetector>());
    if (lotus::checker::tooling::logAtLeast(
            lotus::checker::tooling::LogLevel::Info))
      outs() << "Running buffer overflow checker...\n";
  }

  if (runNullDeref) {
    ae.addDetector(std::make_unique<lotus::analysis::NullptrDerefDetector>());
    if (lotus::checker::tooling::logAtLeast(
            lotus::checker::tooling::LogLevel::Info))
      outs() << "Running null pointer dereference checker...\n";
  }

  if (runUseAfterFree) {
    ae.addDetector(std::make_unique<lotus::analysis::UseAfterFreeDetector>());
    if (lotus::checker::tooling::logAtLeast(
            lotus::checker::tooling::LogLevel::Info))
      outs() << "Running use-after-free checker...\n";
  }

  if (runInvalidFree) {
    ae.addDetector(std::make_unique<lotus::analysis::InvalidFreeDetector>());
    if (lotus::checker::tooling::logAtLeast(
            lotus::checker::tooling::LogLevel::Info))
      outs() << "Running invalid free checker...\n";
  }

  if (runMemLeak) {
    ae.addDetector(std::make_unique<lotus::analysis::MemLeakDetector>());
    if (lotus::checker::tooling::logAtLeast(
            lotus::checker::tooling::LogLevel::Info))
      outs() << "Running memory leak checker...\n";
  }

  // Run the analysis
  ae.runOnModule(M.get());
  stats.emit();

  const int reportStatus = lotus::checker::tooling::emitCheckerReports(
      mgr, {lotus::checker::tooling::Verbose});
  if (reportStatus != lotus::checker::tooling::EXIT_SUCCESS_CODE) {
    return reportStatus;
  }

  if (lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Info))
    outs() << "\n=== Analysis Complete ===\n";

  return lotus::checker::tooling::EXIT_SUCCESS_CODE;
}
