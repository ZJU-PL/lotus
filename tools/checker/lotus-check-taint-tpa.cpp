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
#include <cstdlib>

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
// This is a closed-world research baseline: incomplete models are errors.
static bool validateModels(const Module &module,
                           const tpa::SemiSparsePointerAnalysis &pointer,
                           const annotation::ExternalModRefTable &modref,
                           const annotation::ExternalTaintTable &taint) {
  bool valid = true;
  for (const auto *function : tpa::taint::reachableFunctions(module, pointer))
    for (const auto *block : llvm::depth_first(&function->getEntryBlock()))
      for (const auto &inst : *block) {
        auto *call = dyn_cast<CallBase>(&inst);
        if (!call)
          continue;
        for (auto *target : pointer.getCallees(call)) {
          if (!target->isDeclaration())
            continue;
          auto *mr = modref.lookup(target->getName());
          auto *ts = taint.lookup(target->getName());
          if (!mr || !ts) {
            errs() << "Missing " << (!mr ? "mod/ref" : "taint") << " model for "
                   << target->getName() << "\n";
            valid = false;
            continue;
          }
          auto checkPosition = [&](annotation::APosition pos, bool memory) {
            if (pos.isReturnPosition()) {
              // Legacy specifications contain value-return effects for void
              // LLVM memory intrinsics. Match the pointer client's no-op.
              if (call->getType()->isVoidTy() && !memory)
                return;
              if (call->getType()->isVoidTy() ||
                  (memory && !call->getType()->isPointerTy()))
                valid = false;
              return;
            }
            auto arg = pos.getAsArgPosition();
            unsigned index = arg.getArgIndex();
            if (!arg.isAfterArgPosition()) {
              if (index >= call->arg_size() ||
                  (memory &&
                   !call->getArgOperand(index)->getType()->isPointerTy()))
                valid = false;
            } else {
              for (unsigned i = index; i < call->arg_size(); ++i)
                if (memory && !call->getArgOperand(i)->getType()->isPointerTy())
                  valid = false;
            }
          };
          for (const auto &effect : *mr)
            checkPosition(effect.getPosition(), true);
          for (const auto &entry : *ts) {
            using namespace annotation;
            if (entry.getEntryEnd() == TEnd::Source) {
              const auto &source = entry.getAsSourceEntry();
              checkPosition(source.getTaintPosition(),
                            source.getTaintClass() != TClass::ValueOnly);
            } else if (entry.getEntryEnd() == TEnd::Sink) {
              const auto &sink = entry.getAsSinkEntry();
              checkPosition(sink.getArgPosition(),
                            sink.getTaintClass() != TClass::ValueOnly);
            } else {
              const auto &pipe = entry.getAsPipeEntry();
              checkPosition(pipe.getSrcPosition(),
                            pipe.getSrcClass() != TClass::ValueOnly);
              checkPosition(pipe.getDstPosition(),
                            pipe.getDstClass() != TClass::ValueOnly);
            }
          }
        }
      }
  if (!valid)
    errs() << "TPA taint requires complete models with valid call argument "
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
  if (verifyModule(*module, &errs()) ||
      !tpa::taint::validateModule(*module, errs()))
    return 2;
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
  if (verifyModule(*module, &errs()))
    return 2;
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
  if (!validateModels(*module, pointer, modref, taint))
    return 2;
  tpa::taint::DefUseModuleBuilder builder(pointer);
  builder.loadExternalModRefTable(mrFile.c_str());
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
  tpa::taint::TaintAnalysis analysis(pointer);
  analysis.setExternalTaintTable(std::move(taint));
  analysis.runOnDefUseModule(graph, Track);
  auto done = Clock::now();
  if (Track) {
    using namespace util::io;
    for (const auto &site : analysis.getLossSites())
      outs() << "Precision loss at " << site << "\n";
  }
  if (lotus::checker::tooling::statsEnabled()) {
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
