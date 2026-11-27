/*
 * Andersen's Pointer Analysis Driver
 *
 * This file implements the main driver for running Andersen's pointer analysis
 * on LLVM bitcode files. It parses command-line options, loads the input
 * module, runs the Andersen analysis, and outputs the results.
 *
 * Andersen's analysis is a subset-based, flow-insensitive, field-sensitive
 * pointer analysis algorithm. It supports both context-insensitive and
 * context-sensitive variants (k-CFA, where 0 <= k <= 32).
 */

#include "Alias/InclusionBased/SparrowAA/Andersen.h"
#include "Alias/InclusionBased/SparrowAA/AndersenAA.h"
#include "Alias/InclusionBased/SparrowAA/Log.h"
#include "Alias/InclusionBased/SparrowAA/ResultUtils.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ModuleSlotTracker.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias::tools;

enum class LogLevel { TRACE, DEBUG, INFO, WARN, ERR, OFF };

// Define option category for sparrow-aa tool options
static cl::OptionCategory
    SparrowAACategory("Sparrow-AA Options",
                      "Options for the Sparrow-AA pointer analysis tool");

// Andersen analysis options (defined in
// lib/Alias/InclusionBased/SparrowAA/Andersen.cpp)
extern cl::OptionCategory AndersenCategory;
extern cl::opt<unsigned> AndersenKContext;
extern cl::opt<bool> AndersenUseBDDPointsTo;
// Adaptive CS options (defined in ConstraintOptimize.cpp)
extern cl::opt<bool> EnableAdaptiveCS;
extern cl::opt<float> AdaptiveCSThreshold;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required,
                                          cl::value_desc("filename"),
                                          cl::cat(SparrowAACategory));
static cl::opt<bool>
    PrintPointsTo("print-pts",
                  cl::desc("Print points-to information for all pointers"),
                  cl::init(false), cl::cat(SparrowAACategory));
static cl::opt<bool> PrintGlobalsOnly(
    "print-globals-only",
    cl::desc("Print points-to information for global variables only"),
    cl::init(false), cl::cat(SparrowAACategory));
static cl::opt<bool>
    PrintAllocSites("print-alloc-sites",
                    cl::desc("Print all allocation sites identified"),
                    cl::init(false), cl::cat(SparrowAACategory));
static cl::opt<bool> PrintAliasQueries(
    "print-alias-queries",
    cl::desc("Perform and print alias queries between pointers"),
    cl::init(false), cl::cat(SparrowAACategory));
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false),
                             cl::cat(SparrowAACategory));
static cl::opt<bool> OnlyStatistics("s", cl::desc("Only output statistics"),
                                    cl::init(false),
                                    cl::cat(SparrowAACategory));
static cl::opt<bool>
    VerifyInput("verify", cl::desc("Verify input module before analysis"),
                cl::init(true), cl::cat(SparrowAACategory));
static cl::opt<bool> PrintContextNeed(
    "print-context-need",
    cl::desc("Print per-function adaptive-CS context-need scores (requires "
             "--andersen-adaptive-cs)"),
    cl::init(false), cl::cat(SparrowAACategory));

static cl::opt<LogLevel> LogLevelOpt(
    "log-level", cl::desc("Set the logging level"),
    cl::values(clEnumValN(LogLevel::TRACE, "trace",
                          "Display all messages including trace information"),
               clEnumValN(LogLevel::DEBUG, "debug",
                          "Display all messages including debug information"),
               clEnumValN(LogLevel::INFO, "info",
                          "Display informational messages and above (default)"),
               clEnumValN(LogLevel::WARN, "warn",
                          "Display warnings and errors only"),
               clEnumValN(LogLevel::ERR, "error", "Display errors only"),
               clEnumValN(LogLevel::OFF, "off", "Suppress all log output")),
    cl::init(LogLevel::INFO), cl::cat(SparrowAACategory));
static cl::opt<bool> QuietLogging(
    "quiet",
    cl::desc("Suppress most log output (equivalent to --log-level=off)"),
    cl::init(false), cl::cat(SparrowAACategory));

static void printValue(const Value *V, raw_ostream &OS,
                       const sparrow_aa::ValueNameCache &NameCache) {
  if (V->hasName()) {
    OS << V->getName();
    return;
  }
  auto it = NameCache.find(V);
  if (it != NameCache.end())
    OS << it->second;
  else
    V->printAsOperand(OS, false);
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  volatile unsigned dummy_k = AndersenKContext;
  volatile bool dummy_bdd = AndersenUseBDDPointsTo;
  volatile bool dummy_acs = EnableAdaptiveCS;
  volatile float dummy_thr = AdaptiveCSThreshold;
  (void)dummy_k;
  (void)dummy_bdd;
  (void)dummy_acs;
  (void)dummy_thr;

  cl::HideUnrelatedOptions({&SparrowAACategory, &AndersenCategory});
  cl::ParseCommandLineOptions(
      argc, argv,
      "Andersen's Pointer Analysis Tool\n\n"
      "Subset-based, flow-insensitive, field-sensitive pointer analysis.\n\n"
      "Context Sensitivity:\n"
      "  --andersen-k-cs=<k>      Select call-site sensitivity (0 <= k <= "
      "32):\n"
      "                            0 = context-insensitive (default)\n"
      "                            1 = 1-CFA\n"
      "                            2 = 2-CFA\n"
      "                            ...\n"
      "                           32 = 32-CFA\n"
      "                            k > 32 falls back to k=0 (NoCtx)\n");

  selectGlobalPtsSetImpl(AndersenUseBDDPointsTo ? PtsSetImpl::BDD
                                                : PtsSetImpl::SPARSE_BITVECTOR);

  LogLevel effectiveLevel = QuietLogging ? LogLevel::OFF : LogLevelOpt;
  static const spdlog::level::level_enum levels[] = {
      spdlog::level::trace, spdlog::level::debug, spdlog::level::info,
      spdlog::level::warn,  spdlog::level::err,   spdlog::level::off};
  spdlog::set_level(levels[static_cast<int>(effectiveLevel)]);
  spdlog::set_pattern("%^[%l]%$ %v");

  LLVMContext Context;
  SMDiagnostic Err;

  if (Verbose && !OnlyStatistics)
    errs() << "Loading: " << InputFilename << "\n";

  auto M = loadIRModule(InputFilename, Context, Err, argv[0]);
  if (!M) {
    return 1;
  }
  if (VerifyInput && verifyModule(*M, &errs())) {
    errs() << "Module verification failed\n";
    return 1;
  }

  ContextPolicy policy = getSelectedAndersenContextPolicy();
  if (Verbose && !OnlyStatistics) {
    errs() << "Module: " << M->getName() << " (" << M->getFunctionList().size()
           << " functions, " << M->getGlobalList().size() << " globals)\n"
           << "Context sensitivity: " << policy.name
           << "\nRunning analysis...\n";
  }

  // Construct AndersenAAResult once and reuse its underlying Andersen instance
  // so --print-alias-queries never runs the whole-program analysis twice.
  AndersenAAResult AAResult(*M, policy);
  Andersen &Anders = AAResult.getAndersen();
  if (Verbose && !OnlyStatistics)
    errs() << "Done.\n\n";

  if (!OnlyStatistics) {
    outs() << "\n=== Andersen Analysis Results ===";
    if (policy.name && strcmp(policy.name, "NoCtx") != 0)
      outs() << " (" << policy.name << ")";
    outs() << "\n\n";

    sparrow_aa::ValueNameCache NameCache;
    if (PrintAllocSites || PrintPointsTo || PrintGlobalsOnly)
      NameCache = sparrow_aa::buildValueNameCache(*M);

    if (PrintAllocSites) {
      std::vector<const Value *> allocSites;
      Anders.getAllAllocationSites(allocSites);
      outs() << "--- Allocation Sites (" << allocSites.size() << ") ---\n\n";
      for (const Value *V : allocSites) {
        outs() << "  ";
        printValue(V, outs(), NameCache);
        if (auto *GV = dyn_cast<GlobalVariable>(V))
          outs() << " [global, " << (GV->isConstant() ? "const]" : "mutable]");
        else if (auto *AI = dyn_cast<AllocaInst>(V)) {
          if (AI->getParent() && AI->getFunction())
            outs() << " [stack, in " << AI->getFunction()->getName() << "]";
          else
            outs() << " [stack]";
        } else if (isa<CallBase>(V))
          outs() << " [heap]";
        else if (isa<Function>(V))
          outs() << " [function]";
        outs() << "\n";
      }
      outs() << "\n";
    }

    if (PrintPointsTo || PrintGlobalsOnly) {
      outs() << "--- Points-To Information ---\n\nGlobal Variables:\n";
      bool found = false;
      for (const GlobalVariable &GV : M->globals())
        if (GV.getType()->isPointerTy()) {
          found = true;
          sparrow_aa::printPointsToSet(&GV, Anders, outs(), NameCache);
        }
      if (!found)
        outs() << "  (none)\n";
      outs() << "\n";

      if (PrintPointsTo && !PrintGlobalsOnly) {
        for (const Function &F : *M) {
          if (F.isDeclaration())
            continue;
          bool header = false;
          for (const Argument &A : F.args())
            if (A.getType()->isPointerTy()) {
              if (!header) {
                outs() << "Function: " << F.getName() << "\n";
                header = true;
              }
              outs() << "  Arg: ";
              sparrow_aa::printPointsToSet(&A, Anders, outs(), NameCache);
            }
          for (const BasicBlock &BB : F)
            for (const Instruction &I : BB)
              if (I.getType()->isPointerTy()) {
                if (!header) {
                  outs() << "Function: " << F.getName() << "\n";
                  header = true;
                }
                sparrow_aa::printPointsToSet(&I, Anders, outs(), NameCache);
              }
          if (header)
            outs() << "\n";
        }
      }
    }

    if (PrintAliasQueries) {
      sparrow_aa::performAliasQueries(*M, AAResult, outs());
    }
    outs() << "\nAnalysis completed.\n";
  }

  if (PrintContextNeed) {
    const auto &scores = Anders.getContextNeedMap();
    if (scores.empty()) {
      outs() << "\n--- Context-Need Scores ---\n"
             << "(none — run with --andersen-adaptive-cs and k>=1)\n";
    } else {
      // Collect and sort by score descending for easy reading
      std::vector<std::pair<float, std::string>> rows;
      rows.reserve(scores.size());
      for (const auto &kv : scores)
        rows.emplace_back(kv.second, kv.first->getName().str());
      std::sort(rows.begin(), rows.end(),
                [](const auto &a, const auto &b) { return a.first > b.first; });

      outs() << "\n--- Context-Need Scores (threshold=" << AdaptiveCSThreshold
             << ") ---\n";
      for (const auto &[score, name] : rows)
        outs() << llvm::format("  %-40s  %.3f  %s\n", name.c_str(), score,
                               score >= AdaptiveCSThreshold ? "[deepened]"
                                                            : "[skipped]");
    }
  }

  if (OnlyStatistics || Verbose) {
    const auto &S = Anders.getStats();
    errs() << "\n=== Statistics ===\n";
    errs() << "  Nodes:                  " << S.numNodes << "\n"
           << "  Constraints (raw):      " << S.numConstraintsBeforeOpt << "\n"
           << "  Constraints (opt):      " << S.numConstraintsAfterOpt << "\n"
           << "  Solving iterations:     " << S.numSolvingIterations << "\n"
           << "  Non-empty pts sets:     " << S.numNonEmptyPtsSets << "\n"
           << "  Max pts set size:       " << S.maxPtsSetSize << "\n"
           << "  Avg pts set size:       " << S.avgPtsSetSize << "\n";
    PrintStatistics(errs());
  }

  return 0;
}
