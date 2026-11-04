#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/CheckerDriver.h"
#include "Checker/Framework/CheckerRegistry.h"
#include "Checker/Framework/CheckerSpecLoader.h"
#include "Checker/Framework/ReportOptions.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/Tooling/CheckerOptions.h"
#include "Checker/Tooling/CheckerReport.h"
#include "Checker/Tooling/CheckerToolEntrypoints.h"

#include "Checker/GSAF/API/VulnerabilityRegistry.h"

#include <array>
#include <string>
#include <vector>

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

// ---------------------------------------------------------------------------
// Engine descriptor table
// ---------------------------------------------------------------------------

namespace {

struct EngineDescriptor {
  StringRef name;
  StringRef summary;
  lotus::checker::EngineKind kind;
  cl::SubCommand &(*subcommand)();
  int (*run)(const char *);
};

const std::array<EngineDescriptor, 10> &engineDescriptors() {
  static const std::array<EngineDescriptor, 10> descriptors = {{
      {"generic", "Registry-backed declarative checkers",
       lotus::checker::EngineKind::Declarative,
       lotus::checker::tooling::genericSubCommand, runGenericCheckerTool},
      {"ae", "Abstract execution", lotus::checker::EngineKind::AE,
       lotus::checker::tooling::aeSubCommand, runAECheckerTool},
      {"kint", "Integer bug detection", lotus::checker::EngineKind::KINT,
       lotus::checker::tooling::kintSubCommand, runKintCheckerTool},
      {"taint", "IFDS taint analysis", lotus::checker::EngineKind::Taint,
       lotus::checker::tooling::taintSubCommand, runTaintCheckerTool},
      {"concur", "Concurrency checking",
       lotus::checker::EngineKind::Concurrency,
       lotus::checker::tooling::concurrencySubCommand,
       runConcurrencyCheckerTool},
      {"pulse", "Pulse memory-safety analysis",
       lotus::checker::EngineKind::Pulse,
       lotus::checker::tooling::pulseSubCommand, runPulseCheckerTool},
      {"fitx", "FiTx typestate analysis", lotus::checker::EngineKind::FiTx,
       lotus::checker::tooling::fitxSubCommand, runFiTxCheckerTool},
      {"saber", "Sparse value-flow checking",
       lotus::checker::EngineKind::Saber,
       lotus::checker::tooling::saberSubCommand, runSaberCheckerTool},
      {"gsaf", "Guarded value-flow analysis", lotus::checker::EngineKind::GSAF,
       lotus::checker::tooling::gsafSubCommand, runGSAFCheckerTool},
      {"symex", "Symbolic execution", lotus::checker::EngineKind::SymExec,
       lotus::checker::tooling::symexSubCommand, runSymExCheckerTool},
  }};
  return descriptors;
}

const EngineDescriptor *findEngine(StringRef name) {
  for (const auto &d : engineDescriptors())
    if (d.name == name)
      return &d;
  return nullptr;
}

bool isKnownEngine(StringRef name) { return findEngine(name) != nullptr; }

/// Validate that all options are known to the target subcommand.
/// Returns an error for the first unrecognized option found.
Error validateOptions(ArrayRef<std::string> args) {
  cl::SubCommand *sub = &*cl::TopLevelSubCommand;
  if (args.size() > 1 && isKnownEngine(args[1]))
    sub = &findEngine(args[1])->subcommand();

  static const StringRef helpFlags[] = {"help", "help-hidden", "help-list",
                                        "help-list-hidden", "version", "h"};
  bool afterSentinel = false;
  unsigned positionalCount = 0;
  for (size_t i = 1; i < args.size(); ++i) {
    StringRef a(args[i]);
    // Skip the subcommand token inserted by normalization.
    if (i == 1 && isKnownEngine(a))
      continue;
    if (a == "--") {
      afterSentinel = true;
      continue;
    }
    if (afterSentinel) {
      ++positionalCount;
      continue;
    }
    if (!a.startswith("-") || a == "-") {
      ++positionalCount;
      continue;
    }
    StringRef spelling = a.startswith("--") ? a.drop_front(2) : a.drop_front(1);
    spelling = spelling.split('=').first;
    if (spelling.empty() || sub->OptionsMap.count(spelling))
      continue;
    if (llvm::is_contained(helpFlags, spelling))
      continue;
    StringRef prefix = a.startswith("--") ? "--" : "-";
    return createStringError(inconvertibleErrorCode(),
                             "Unknown command line argument '%s%s'.",
                             prefix.str().c_str(),
                             spelling.str().c_str());
  }
  if (positionalCount > 1) {
    return createStringError(inconvertibleErrorCode(),
                             "Too many positional arguments specified!");
  }
  return Error::success();
}

StringRef cliEngineName(lotus::checker::EngineKind engine) {
  for (const auto &d : engineDescriptors())
    if (d.kind == engine)
      return d.name;
  llvm_unreachable("unhandled checker engine");
}

void printEngineHelp(const EngineDescriptor &desc, bool showHidden) {
  cl::SubCommand &sub = desc.subcommand();
  outs() << "OVERVIEW: Lotus " << desc.name << " engine\n\n"
         << "USAGE: lotus-check --engine=" << desc.name
         << " [options] <input bitcode file>\n\nOPTIONS:\n\n";

  std::vector<cl::Option *> opts;
  for (const auto &entry : sub.OptionsMap) {
    cl::Option *o = entry.second;
    if (!o || o->isPositional())
      continue;
    if (!showHidden && o->getOptionHiddenFlag() != cl::NotHidden)
      continue;
    opts.push_back(o);
  }
  llvm::sort(opts, [](const cl::Option *a, const cl::Option *b) {
    return a->ArgStr < b->ArgStr;
  });
  size_t width = 0;
  for (const auto *o : opts)
    width = std::max(width, o->getOptionWidth());
  for (const auto *o : opts)
    o->printOptionInfo(width);

  if (desc.kind != lotus::checker::EngineKind::Declarative) {
    auto checks = lotus::checker::getBuiltinNativeChecks(desc.kind);
    if (!checks.empty()) {
      outs() << "\nCHECKS:\n\n";
      for (const auto &c : checks) {
        outs() << "  " << formatv("{0,-28}", c.id) << c.title;
        if (c.default_enabled)
          outs() << " (default)";
        outs() << "\n";
      }
    }
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Generic-engine CLI options
// ---------------------------------------------------------------------------

static cl::OptionCategory GenericSelectionCategory(
    "Generic Checker Selection Options");
static cl::OptionCategory GenericExecutionCategory(
    "Generic Checker Execution Options");
static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                  cl::value_desc("bitcode"), cl::init(""),
                  cl::cat(GenericExecutionCategory),
                  cl::sub(*cl::TopLevelSubCommand),
                  cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<bool>
    ListCheckers("list-checkers",
                 cl::desc("List available checker ids and exit"),
                 cl::cat(GenericSelectionCategory), cl::init(false),
                 cl::sub(*cl::TopLevelSubCommand),
                 cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<std::string> CategoryFilter(
    "generic.category", cl::desc("Run only checkers in the given category"),
    cl::value_desc("category"), cl::init(""), cl::cat(GenericSelectionCategory),
    cl::sub(*cl::TopLevelSubCommand),
    cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<std::string> BuiltinSpecDir(
    "generic.spec-dir",
    cl::desc("Load declarative checker specs from this directory"),
    cl::value_desc("dir"), cl::init(""), cl::cat(GenericExecutionCategory),
    cl::sub(*cl::TopLevelSubCommand),
    cl::sub(lotus::checker::tooling::genericSubCommand()));

// ---------------------------------------------------------------------------
// Registry construction (shared by generic engine and --list-checkers)
// ---------------------------------------------------------------------------

namespace {

Expected<lotus::checker::CheckerRegistry> buildRegistry() {
  lotus::checker::CheckerRegistry registry;
  if (auto error = lotus::checker::registerBuiltinNativeCheckers(registry))
    return std::move(error);

  std::string specDir;
  if (BuiltinSpecDir.getNumOccurrences() != 0)
    specDir = BuiltinSpecDir;
  else if (auto env = sys::Process::GetEnv("LOTUS_CHECKER_SPEC_DIR"))
    specDir = *env;
  else if (sys::fs::is_directory(LOTUS_INSTALL_CHECKER_SPEC_DIR))
    specDir = LOTUS_INSTALL_CHECKER_SPEC_DIR;
  else
    specDir = LOTUS_SOURCE_CHECKER_SPEC_DIR;

  lotus::checker::CheckerSpecLoader loader;
  auto specs_or = loader.loadFromDirectory(specDir);
  if (!specs_or)
    return specs_or.takeError();
  for (const auto &spec : *specs_or)
    if (auto error = registry.registerDeclarative(spec))
      return std::move(error);

  return registry;
}

} // namespace

// ---------------------------------------------------------------------------
// Generic engine entry point
// ---------------------------------------------------------------------------

int runGenericCheckerTool(const char *argv0) {
  (void)lotus::checker::tooling::statsEnabled();
  auto registry_or = buildRegistry();
  if (!registry_or) {
    logAllUnhandledErrors(registry_or.takeError(), errs(), "");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  auto registry = std::move(*registry_or);

  if (ListCheckers) {
    outs() << "ID\tENGINE\tMODE\tDEFAULT\tCATEGORY\tTITLE\n";
    for (const auto *desc : registry.list()) {
      if (desc->isDeclarative()) {
        outs() << desc->metadata.id << "\tgeneric\tgeneric\t"
               << (desc->metadata.default_enabled ? "yes" : "no") << "\t"
               << desc->metadata.category << "\t" << desc->metadata.title
               << "\n";
        continue;
      }
      for (const auto &check :
           lotus::checker::getBuiltinNativeChecks(desc->metadata.engine)) {
        outs() << check.id << "\t" << cliEngineName(desc->metadata.engine)
               << "\tnative\t" << (check.default_enabled ? "yes" : "no") << "\t"
               << desc->metadata.category << "\t" << check.title << "\n";
      }
    }
    return lotus::checker::tooling::EXIT_SUCCESS_CODE;
  }

  if (InputFilename.empty()) {
    errs() << "error: input bitcode file is required unless --list-checkers is "
              "used\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  LLVMContext context;
  SMDiagnostic error;
  auto module = parseIRFile(InputFilename, error, context);
  if (!module) {
    error.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("generic", *module, mgr);

  std::vector<const lotus::checker::CheckerDescriptor *> selection;
  auto requestedOr = lotus::checker::tooling::parseCheckSelection("generic");
  if (!requestedOr) {
    logAllUnhandledErrors(requestedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  const bool selectAll = lotus::checker::tooling::hasExplicitCheckSelection() &&
                         *requestedOr == std::vector<std::string>{"all"};
  if (lotus::checker::tooling::hasExplicitCheckSelection() && !selectAll) {
    for (const std::string &id : *requestedOr) {
      auto descriptor_or = registry.findById(id);
      if (!descriptor_or) {
        logAllUnhandledErrors(descriptor_or.takeError(), errs(), "");
        return lotus::checker::tooling::EXIT_ERROR;
      }
      const auto *descriptor = *descriptor_or;
      if (!descriptor->isDeclarative()) {
        errs() << "error: checker engine '" << descriptor->metadata.id
               << "' cannot run in generic mode\n"
               << "hint: use --engine="
               << cliEngineName(descriptor->metadata.engine)
               << " --checks=<id>\n";
        return lotus::checker::tooling::EXIT_ERROR;
      }
      if (!CategoryFilter.empty() &&
          descriptor->metadata.category != CategoryFilter)
        continue;
      selection.push_back(descriptor);
    }
  } else {
    selection =
        registry.select(CategoryFilter, lotus::checker::EngineKind::Declarative);
    if (!lotus::checker::tooling::hasExplicitCheckSelection() &&
        CategoryFilter.empty()) {
      std::vector<const lotus::checker::CheckerDescriptor *> defaults;
      for (const auto *d : selection)
        if (d->metadata.default_enabled)
          defaults.push_back(d);
      selection = std::move(defaults);
    }
  }

  if (selection.empty()) {
    errs() << "error: no checkers selected\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  lotus::checker::CheckerContext checker_context{*module};
  lotus::checker::CheckerDriver driver(registry, checker_context);
  auto diagnostics_or = driver.run(selection);
  if (!diagnostics_or) {
    logAllUnhandledErrors(diagnostics_or.takeError(), errs(), "");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  if (auto report_error = driver.emitToReportManager(*diagnostics_or)) {
    logAllUnhandledErrors(std::move(report_error), errs(), "");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  stats.emit();
  return lotus::checker::tooling::emitCheckerReports(
      mgr, {lotus::checker::tooling::Verbose});
}

// ---------------------------------------------------------------------------
// main — thin entry point: parse --engine, dispatch to the engine runner
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram stack_trace(argc, argv);
  llvm::InitLLVM init_llvm(argc, argv);
  llvm_shutdown_obj shutdown;
  report_options::initializeReportOptions();
  lotus::gsaf::initializeBuiltinVulnerabilities();

  // Extract --engine=<name> from argv before cl::Parse sees it.
  std::string selectedEngine;
  std::vector<std::string> filteredArgs;
  filteredArgs.push_back(argv[0]);

  for (int i = 1; i < argc; ++i) {
    StringRef arg(argv[i]);
    if (arg == "--")
      break;

    StringRef engineName;
    if (arg.consume_front("--engine=")) {
      engineName = arg;
    } else if (arg == "--engine") {
      if (++i >= argc) {
        errs() << "error: --engine requires a value\n";
        return lotus::checker::tooling::EXIT_ERROR;
      }
      engineName = argv[i];
    } else {
      filteredArgs.push_back(argv[i]);
      continue;
    }

    if (!selectedEngine.empty()) {
      errs() << "error: --engine may only be specified once\n";
      return lotus::checker::tooling::EXIT_ERROR;
    }
    if (!findEngine(engineName)) {
      errs() << "error: unknown engine '" << engineName << "'; available:";
      for (const auto &d : engineDescriptors())
        errs() << " " << d.name;
      errs() << "\n";
      return lotus::checker::tooling::EXIT_ERROR;
    }
    selectedEngine = engineName.str();
  }

  // Collect remaining args (after "--" sentinel, if any).
  for (int i = 1; i < argc; ++i) {
    if (StringRef(argv[i]) == "--") {
      for (int j = i; j < argc; ++j)
        filteredArgs.push_back(argv[j]);
      break;
    }
  }

  // Build the argv for cl::ParseCommandLineOptions with the engine as a
  // subcommand token.
  std::vector<std::string> normalizedArgs;
  normalizedArgs.push_back(filteredArgs[0]);
  if (!selectedEngine.empty())
    normalizedArgs.push_back(selectedEngine);
  for (size_t i = 1; i < filteredArgs.size(); ++i)
    normalizedArgs.push_back(filteredArgs[i]);

  std::vector<char *> argvPtrs;
  for (auto &s : normalizedArgs)
    argvPtrs.push_back(s.data());
  argvPtrs.push_back(nullptr);
  int newArgc = static_cast<int>(normalizedArgs.size());

  if (Error e = validateOptions(normalizedArgs)) {
    logAllUnhandledErrors(std::move(e), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Intercept --help before LLVM's parser (which calls exit(0)).
  // This lets us append native check IDs for engine subcommands.
  if (!selectedEngine.empty()) {
    bool showHidden = false;
    bool wantsHelp = false;
    for (const auto &arg : normalizedArgs) {
      StringRef a(arg);
      if (a == "--help" || a == "--help-list") {
        wantsHelp = true;
      } else if (a == "--help-hidden" || a == "--help-list-hidden") {
        wantsHelp = true;
        showHidden = true;
      }
    }
    if (wantsHelp) {
      printEngineHelp(*findEngine(selectedEngine), showHidden);
      return lotus::checker::tooling::EXIT_SUCCESS_CODE;
    }
  }

  if (!cl::ParseCommandLineOptions(
          newArgc, argvPtrs.data(),
          "Lotus checker front-end\n"
          "  Usage: lotus-check --engine=<name> [options] <input bitcode>\n")) {
    return lotus::checker::tooling::EXIT_ERROR;
  }

  if (!lotus::checker::tooling::validateReportOptions())
    return lotus::checker::tooling::EXIT_ERROR;

  lotus::checker::tooling::configureCommonLogging();

  // Dispatch to the selected engine.
  if (!selectedEngine.empty()) {
    const auto *desc = findEngine(selectedEngine);
    return desc->run(argvPtrs[0]);
  }

  // No engine: check for top-level actions (--list-checkers).
  if (ListCheckers)
    return runGenericCheckerTool(argvPtrs[0]);

  errs() << "error: no engine selected\n"
         << "hint: use --engine=<name> or --list-checkers\n"
         << "\navailable engines:\n";
  for (const auto &d : engineDescriptors())
    errs() << "  " << formatv("{0,-8}", d.name) << " " << d.summary << "\n";
  return lotus::checker::tooling::EXIT_ERROR;
}
