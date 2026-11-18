#include "Alias/InclusionBased/TPA/Context/ContextPolicy.h"
#include "Alias/InclusionBased/TPA/Context/KLimitContext.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/TPA/Analysis/TaintAnalysis.h"
#include "Checker/TPA/FrontEnd/DefUseModuleBuilder.h"
#include "Checker/TPA/Output/Printer.h"
#include "Checker/TPA/Output/WriteDotFile.h"
#include "Checker/TPA/Program/Reachability.h"
#include "Checker/Tooling/CheckerOptions.h"
#include "Checker/Tooling/CheckerReport.h"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;
static cl::opt<std::string>
    Input(cl::Positional, cl::Required, cl::desc("<input LLVM IR or bitcode>"),
          cl::sub(lotus::checker::tooling::taintTPASubCommand()));
static cl::opt<unsigned>
    K("taint-tpa.k-limit", cl::init(0),
      cl::desc("Call-string limit shared by pointer and taint analyses"),
      cl::sub(lotus::checker::tooling::taintTPASubCommand()));
static cl::opt<std::string>
    PointerSpec("taint-tpa.ptr-config", cl::init(""),
                cl::desc("External pointer specification"),
                cl::sub(lotus::checker::tooling::taintTPASubCommand()));
static cl::opt<std::string>
    ModRefSpec("taint-tpa.modref-config", cl::init(""),
               cl::desc("External mod/ref specification"),
               cl::sub(lotus::checker::tooling::taintTPASubCommand()));
static cl::opt<std::string> TaintSpec(
    "taint-tpa.taint-config", cl::init(""),
    cl::desc("External taint specification (SOURCE/SINK/PIPE/IGNORE)"),
    cl::sub(lotus::checker::tooling::taintTPASubCommand()));
static cl::opt<bool> NoPrepass(
    "taint-tpa.no-prepass", cl::init(false),
    cl::desc("Skip optional TPA normalization (returns still unified)"),
    cl::sub(lotus::checker::tooling::taintTPASubCommand()));
static cl::opt<bool>
    Track("taint-tpa.track-precision-loss", cl::init(false),
          cl::desc("Trace ambiguous sink findings to precision-loss sites"),
          cl::sub(lotus::checker::tooling::taintTPASubCommand()));
static cl::opt<std::string>
    DotDir("taint-tpa.def-use-dot-dir", cl::init(""),
           cl::desc("Write sparse def-use graphs to this directory"),
           cl::sub(lotus::checker::tooling::taintTPASubCommand()));

static cl::opt<bool>
    StripDebug("taint-tpa.strip-debug", cl::init(false),
               cl::desc("Strip debug info before verification (report source "
                        "locations are lost)"),
               cl::sub(lotus::checker::tooling::taintTPASubCommand()));

static cl::opt<tpa::taint::UnmodeledPolicy> Unmodeled(
    "taint-tpa.unmodeled",
    cl::desc("Policy for missing external models and unresolved calls"),
    cl::values(clEnumValN(tpa::taint::UnmodeledPolicy::Ignore, "ignore",
                          "Assume no additional effects (optimistic)"),
               clEnumValN(tpa::taint::UnmodeledPolicy::Error, "error",
                          "Require all models and call targets")),
    cl::init(tpa::taint::UnmodeledPolicy::Ignore),
    cl::sub(lotus::checker::tooling::taintTPASubCommand()));

struct ExternalModelCoverage {
  struct Symbol {
    size_t calls = 0;
    size_t missingModRef = 0;
    size_t missingTaint = 0;
  };
  std::map<std::string, Symbol> ignored;
  std::set<const CallBase *> ignoredCalls;
  std::set<const CallBase *> unresolvedCalls;
  std::map<std::string, size_t> unresolvedByFunction;
  size_t inlineAsmCalls = 0;
  size_t filteredIndirectTargets = 0;
};

static std::string defaultSpec(StringRef file) {
  SmallString<256> path;
  if (const char *dir = std::getenv("LOTUS_CONFIG_DIR")) {
    path = dir;
    sys::path::append(path, file);
    return path.str().str();
  }
  path = LOTUS_INSTALL_TPA_CONFIG_DIR;
  sys::path::append(path, file);
  if (sys::fs::exists(path))
    return path.str().str();
  path = LOTUS_SOURCE_TPA_CONFIG_DIR;
  sys::path::append(path, file);
  return path.str().str();
}

// Validate all resolved external targets before running the sparse builder.
// Validate available models independently and account for missing ones.
static bool validateModels(const Module &module,
                           const tpa::SemiSparsePointerAnalysis &pointer,
                           const annotation::ExternalModRefTable &modref,
                           const annotation::ExternalTaintTable &taint,
                           ExternalModelCoverage &coverage) {
  bool valid = true;
  for (const auto *function :
       tpa::taint::reachableFunctions(module, pointer, Unmodeled))
    for (const auto *block : llvm::depth_first(&function->getEntryBlock()))
      for (const auto &inst : *block) {
        auto *call = dyn_cast<CallBase>(&inst);
        if (!call || tpa::taint::isDebugIntrinsic(*call))
          continue;
        size_t filtered = 0;
        auto targets = tpa::taint::resolveCallTargets(pointer, *call, Unmodeled,
                                                      nullptr, &filtered);
        coverage.filteredIndirectTargets += filtered;
        if (targets.empty()) {
          coverage.unresolvedCalls.insert(call);
          ++coverage.unresolvedByFunction[function->getName().str()];
          coverage.inlineAsmCalls += call->isInlineAsm();
        }
        for (auto *target : targets) {
          if (!target->isDeclaration() || tpa::taint::isDebugIntrinsic(*target))
            continue;
          auto *mr = modref.lookup(target->getName());
          auto *ts = taint.lookup(target->getName());
          if (!mr || !ts) {
            if (Unmodeled == tpa::taint::UnmodeledPolicy::Error) {
              if (!mr)
                errs() << "Missing mod/ref model for " << target->getName()
                       << "\n";
              if (!ts)
                errs() << "Missing taint model for " << target->getName()
                       << "\n";
              valid = false;
            } else {
              auto &symbol = coverage.ignored[target->getName().str()];
              ++symbol.calls;
              symbol.missingModRef += !mr;
              symbol.missingTaint += !ts;
              coverage.ignoredCalls.insert(call);
            }
          }
          auto checkPosition = [&](const annotation::APosition &pos, bool memory,
                                   StringRef modelKind,
                                   bool pointerRange = false) {
            auto reject = [&](const Twine &reason) {
              errs() << "Invalid " << modelKind << " model for "
                     << target->getName() << " in " << function->getName()
                     << ": " << reason << "\n  " << *call << "\n";
              valid = false;
            };
            if (pos.isReturnPosition()) {
              // Legacy specifications contain value-return effects for void
              // LLVM memory intrinsics. Match the pointer client's no-op.
              if (call->getType()->isVoidTy() && !memory)
                return;
              if (call->getType()->isVoidTy())
                reject("memory effect selects a void return");
              else if (memory && !call->getType()->isPointerTy()) {
                std::string type;
                raw_string_ostream out(type);
                out << *call->getType();
                reject(Twine("memory effect selects non-pointer return type ") +
                       out.str());
              }
              return;
            }
            auto arg = pos.getAsArgPosition();
            unsigned index = arg.getArgIndex();
            if (!arg.isAfterArgPosition() && index >= call->arg_size()) {
              reject(Twine("Arg") + Twine(index) + " is outside the " +
                     Twine(call->arg_size()) + " call arguments");
              return;
            }
            const auto end =
                arg.isAfterArgPosition() ? call->arg_size() : index + 1u;
            for (unsigned i = index; i < end; ++i) {
              if ((pointerRange && arg.isAfterArgPosition()) || !memory ||
                  call->getArgOperand(i)->getType()->isPointerTy())
                continue;
              std::string type;
              raw_string_ostream out(type);
              out << *call->getArgOperand(i)->getType();
              reject(Twine("memory effect ") +
                     (arg.isAfterArgPosition() ? "AfterArg" : "Arg") +
                     Twine(index) + " selects non-pointer argument " +
                     Twine(i) + " of type " + out.str());
            }
          };
          if (mr)
            for (const auto &effect : *mr)
              checkPosition(effect.getPosition(), true, "mod/ref", true);
          if (ts)
            for (const auto &entry : *ts) {
              using namespace annotation;
              if (entry.getEntryEnd() == TEnd::Source) {
                const auto &source = entry.getAsSourceEntry();
                checkPosition(source.getTaintPosition(),
                              source.getTaintClass() != TClass::ValueOnly,
                              "taint source");
              } else if (entry.getEntryEnd() == TEnd::Sink) {
                const auto &sink = entry.getAsSinkEntry();
                checkPosition(sink.getArgPosition(),
                              sink.getTaintClass() != TClass::ValueOnly,
                              "taint sink");
              } else {
                const auto &pipe = entry.getAsPipeEntry();
                checkPosition(pipe.getSrcPosition(),
                              pipe.getSrcClass() != TClass::ValueOnly,
                              "taint pipe source", true);
                checkPosition(pipe.getDstPosition(),
                              pipe.getDstClass() != TClass::ValueOnly,
                              "taint pipe destination");
              }
            }
        }
      }
  if (!valid)
    errs() << "TPA taint external model validation failed; check call argument "
              "positions and types\n";
  return valid;
}

int runTPATaintCheckerTool(const char *argv0) try {
  auto selected = lotus::checker::tooling::resolveChecks(
      lotus::checker::EngineKind::TaintTPA);
  if (!selected) {
    logAllUnhandledErrors(selected.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  LLVMContext llvmContext;
  SMDiagnostic diagnostic;
  auto module = parseIRFile(Input, diagnostic, llvmContext);
  if (!module) {
    diagnostic.print(argv0, errs());
    return 2;
  }
  bool debugStripped = StripDebug;
  if (StripDebug)
    StripDebugInfo(*module);
  bool recoveredDebug = false;
  if (!tpa::taint::verifyInputModule(*module, errs(), recoveredDebug) ||
      !tpa::taint::validateModule(*module, errs()))
    return 2;
  debugStripped |= recoveredDebug;
  if (recoveredDebug && lotus::checker::tooling::logAtLeast(
                            lotus::checker::tooling::LogLevel::Warning))
    errs() << "TPA taint: discarded invalid input debug info; source locations "
              "are unavailable\n";
  std::string ptrFile =
      PointerSpec.empty() ? defaultSpec("ptr.spec") : PointerSpec.getValue();
  std::string mrFile =
      ModRefSpec.empty() ? defaultSpec("modref.spec") : ModRefSpec.getValue();
  std::string taintFile =
      TaintSpec.empty() ? defaultSpec("taint.spec") : TaintSpec.getValue();
  for (const auto &file : {ptrFile, mrFile, taintFile})
    if (!sys::fs::exists(file)) {
      errs() << "Cannot read specification: " << file << "\n";
      return 2;
    }
  using Clock = std::chrono::steady_clock;
  auto started = Clock::now();
  context::KLimitContext::setLimit(K);
  context::setContextStrategy(context::ContextStrategy::KLimit);
  tpa::taint::prepareModule(*module, !NoPrepass);
  if (verifyModule(*module, &errs())) {
    errs() << "TPA preprocessing produced invalid LLVM IR\n";
    return 2;
  }
  tpa::SemiSparseProgramBuilder pointerBuilder;
  auto program = pointerBuilder.runOnModule(*module);
  tpa::SemiSparsePointerAnalysis pointer;
  pointer.loadExternalPointerTable(ptrFile.c_str());
  pointer.runOnProgram(program);
  auto pointerDone = Clock::now();
  annotation::ExternalModRefTable modref;
  if (!annotation::ExternalModRefTable::loadFromFile(mrFile.c_str(), modref))
    return 2;
  annotation::ExternalTaintTable taint;
  auto taintBuffer = MemoryBuffer::getFile(taintFile);
  if (!taintBuffer) {
    errs() << "Cannot read taint specification: " << taintFile << "\n";
    return 2;
  }
  if (!annotation::ExternalTaintTable::parse((*taintBuffer)->getBuffer(), taint,
                                             errs()))
    return 2;
  ExternalModelCoverage coverage;
  if (!validateModels(*module, pointer, modref, taint, coverage))
    return 2;
  if (!coverage.ignored.empty() &&
      lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Warning))
    errs() << "TPA taint: ignoring missing external models for "
           << coverage.ignored.size() << " symbols at "
           << coverage.ignoredCalls.size()
           << " call sites; --analysis-stats lists symbol counts\n";
  tpa::taint::DefUseModuleBuilder builder(pointer, Unmodeled);
  builder.setExternalModRefTable(std::move(modref));
  auto graph = builder.buildDefUseModule(*module);
  auto graphDone = Clock::now();
  if (!DotDir.empty()) {
    if (auto error = sys::fs::create_directories(DotDir)) {
      errs() << "Cannot create graph directory: " << error.message() << "\n";
      return 2;
    }
    unsigned index = 0;
    // Function names may contain path separators; use numeric filenames.
    for (const auto &function : graph) {
      SmallString<256> path(DotDir.getValue());
      sys::path::append(path, std::to_string(index++) + ".dot");
      if (!util::io::writeDotFile(path.c_str(), function))
        return 2;
    }
  }
  tpa::taint::TaintAnalysis analysis(pointer, Unmodeled);
  analysis.setExternalTaintTable(std::move(taint));
  analysis.runOnDefUseModule(graph, Track);
  auto done = Clock::now();
  // Include context-specific resolution failures as well as merged-target
  // failures found during graph construction, without counting revisits.
  for (const auto *call : analysis.getIgnoredUnresolvedCalls())
    if (coverage.unresolvedCalls.insert(call).second) {
      ++coverage.unresolvedByFunction[call->getFunction()->getName().str()];
      coverage.inlineAsmCalls += call->isInlineAsm();
    }
  if (!coverage.unresolvedCalls.empty() &&
      lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Warning))
    errs() << "TPA taint: ignoring " << coverage.unresolvedCalls.size()
           << " unresolved call sites; --analysis-stats lists caller counts\n";

  if (Track) {
    using namespace util::io;
    for (const auto &site : analysis.getLossSites())
      outs() << "Precision loss at " << site << "\n";
  }
  const char *policyName =
      Unmodeled == tpa::taint::UnmodeledPolicy::Ignore ? "ignore" : "error";
  if (lotus::checker::tooling::statsEnabled()) {
    outs() << "unmodeled_policy=" << policyName
           << " ignored_external_symbols=" << coverage.ignored.size()
           << " ignored_external_calls=" << coverage.ignoredCalls.size()
           << " ignored_unresolved_calls=" << coverage.unresolvedCalls.size()
           << " ignored_inline_asm_calls=" << coverage.inlineAsmCalls
           << " filtered_indirect_targets=" << coverage.filteredIndirectTargets
           << "\n";
    for (const auto &entry : coverage.unresolvedByFunction)
      outs() << "ignored_unresolved caller=" << entry.first
             << " call_sites=" << entry.second << "\n";
    for (const auto &entry : coverage.ignored)
      outs() << "ignored_external symbol=" << entry.first
             << " call_sites=" << entry.second.calls
             << " missing_modref=" << entry.second.missingModRef
             << " missing_taint=" << entry.second.missingTaint << "\n";
    const auto &pointerStats = pointer.getStatistics();
    outs() << "pointer_evaluations=" << pointerStats.evaluations
           << " pointer_initialization_seconds="
           << pointerStats.initializationSeconds
           << " pointer_solve_seconds=" << pointerStats.solveSeconds
           << " debug_stripped=" << (debugStripped ? 1 : 0) << "\n";
    const auto &s = analysis.getStatistics();
    auto seconds = [](auto a, auto b) {
      return std::chrono::duration<double>(b - a).count();
    };
    outs() << "raw_sink_violations=" << s.violations << " k_limit=" << K
           << " top_edges=" << s.topEdges << " memory_edges=" << s.memoryEdges
           << " evaluations=" << s.evaluations << " contexts=" << s.contexts
           << " value_facts=" << s.valueFacts
           << " memory_states=" << s.memoryStates << " sinks=" << s.sinks
           << " loss_sites=" << analysis.getLossSites().size() << "\n"
           << "pointer_pipeline_seconds=" << seconds(started, pointerDone)
           << " graph_seconds=" << seconds(pointerDone, graphDone)
           << " taint_and_tracking_seconds=" << seconds(graphDone, done)
           << "\n";
  }
  BugReportMgr &manager = BugReportMgr::get_instance();
  const int bugType = manager.register_bug_type(
      "TPA Taint Violation", BugDescription::BI_HIGH,
      BugDescription::BC_SECURITY, "Tainted data reaches a configured sink");
  for (const auto &entry : analysis.getViolations()) {
    const auto *instruction =
        entry.first.getDefUseInstruction()->getInstruction();
    std::string contextText;
    raw_string_ostream contextStream(contextText);
    using namespace util::io;
    contextStream << *entry.first.getContext();
    contextStream.flush();
    for (const auto &violation : entry.second) {
      auto *report = new BugReport(bugType);
      report->append_step(const_cast<Instruction *>(instruction),
                          "Tainted data reaches sink via arg" +
                              std::to_string(violation.argPos) +
                              " in context " + contextText,
                          0, {NodeTag::CALL_SITE}, "sink");
      report->set_conf_score(80);
      report->add_metadata("engine", "taint-tpa");
      report->add_metadata("debug_stripped", debugStripped ? "true" : "false");
      report->add_metadata("unmodeled_policy", policyName);
      report->add_metadata("ignored_external_symbols",
                           std::to_string(coverage.ignored.size()));
      report->add_metadata("ignored_external_calls",
                           std::to_string(coverage.ignoredCalls.size()));
      report->add_metadata("ignored_unresolved_calls",
                           std::to_string(coverage.unresolvedCalls.size()));
      report->add_metadata("ignored_inline_asm_calls",
                           std::to_string(coverage.inlineAsmCalls));
      report->add_metadata("filtered_indirect_targets",
                           std::to_string(coverage.filteredIndirectTargets));
      report->add_metadata("context", contextText);
      report->add_metadata("argument", std::to_string(violation.argPos));
      manager.insert_report(bugType, report, true);
    }
  }
  return lotus::checker::tooling::emitCheckerReports(
      manager, {lotus::checker::tooling::Verbose});
} catch (const std::exception &error) {
  errs() << "TPA taint: " << error.what() << "\n";
  return 2;
}
