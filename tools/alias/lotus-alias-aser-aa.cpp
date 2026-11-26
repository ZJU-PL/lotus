/*
 * AserPTA: Pointer Analysis Tool
 *
 * A high-performance pointer analysis tool supporting multiple context
 * sensitivities and solver algorithms.
 */

#include "Alias/InclusionBased/AserPTA/PTADriver.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/KCallSite.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/KOrigin.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldInsensitive/FIMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/DeepPropagation.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PartialUpdateSolver.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/BDDPts.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/PointsToSelector.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/WavePropagation.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/Infrastructure/Spec/AliasSpecManager.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>

using namespace aser;
using namespace llvm;
using namespace std;
using namespace lotus::alias;
using namespace lotus::alias::tools;

// Command-line options
static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<std::string> AnalysisMode(
    "analysis-mode",
    cl::desc("Analysis mode: ci (context-insensitive), 1-cfa, 2-cfa, origin"),
    cl::init("ci"), cl::value_desc("mode"));

static cl::opt<std::string>
    SolverType("solver", cl::desc("Solver type: basic, wave, deep"),
               cl::init("wave"), cl::value_desc("solver"));

static cl::opt<bool>
    FieldSensitive("field-sensitive",
                   cl::desc("Use field-sensitive memory model"),
                   cl::init(true));

static cl::opt<bool> DumpStats("dump-stats",
                               cl::desc("Print analysis statistics"),
                               cl::init(true));

static cl::opt<std::string> OutputFile("o", cl::desc("Output file for results"),
                                       cl::value_desc("filename"));
static cl::opt<std::string> WritePointsTo(
    "write-ander",
    cl::desc("Write AserPTA points-to results and node layout to a file"),
    cl::init(""), cl::value_desc("filename"));
static cl::opt<std::string> ReadPointsTo(
    "read-ander",
    cl::desc("Load AserPTA points-to results and rebuild indirect-call edges"),
    cl::init(""), cl::value_desc("filename"));

// Config file options are defined in CLIUtils.cpp

// Type aliases for analysis configurations
using Origin = KOrigin<1>;

template <typename ctx, typename pts>
using FSModel = DefaultLangModel<ctx, FSMemModel<ctx>, pts>;

template <typename ctx, typename pts>
using FIModel = DefaultLangModel<ctx, FIMemModel<ctx>, pts>;

// Solver type definitions
template <typename pts>
using CIWaveSolver = WavePropagation<FSModel<NoCtx, pts>>;
template <typename pts>
using CIDeepSolver = DeepPropagation<FSModel<NoCtx, pts>>;
template <typename pts>
using CIBasicSolver = PartialUpdateSolver<FSModel<NoCtx, pts>>;

template <typename pts>
using CS1WaveSolver = WavePropagation<FSModel<KCallSite<1>, pts>>;
template <typename pts>
using CS1DeepSolver = DeepPropagation<FSModel<KCallSite<1>, pts>>;
template <typename pts>
using CS1BasicSolver = PartialUpdateSolver<FSModel<KCallSite<1>, pts>>;

template <typename pts>
using CS2WaveSolver = WavePropagation<FSModel<KCallSite<2>, pts>>;
template <typename pts>
using CS2DeepSolver = DeepPropagation<FSModel<KCallSite<2>, pts>>;
template <typename pts>
using CS2BasicSolver = PartialUpdateSolver<FSModel<KCallSite<2>, pts>>;

template <typename pts>
using OriginWaveSolver = WavePropagation<FSModel<Origin, pts>>;
template <typename pts>
using OriginDeepSolver = DeepPropagation<FSModel<Origin, pts>>;
template <typename pts>
using OriginBasicSolver = PartialUpdateSolver<FSModel<Origin, pts>>;

#include <iostream>
#define determinePts(block)                                                    \
  {                                                                            \
    if (ConfigUseBDDPts) {                                                     \
      if (ConfigBDDPtsReorder) {                                               \
        const auto &methodName = ConfigBDDPtsReorderMethod.getValue();         \
        BDDAndersPtsSet::ReorderingMethod method =                             \
            BDDAndersPtsSet::ReorderingMethod::Sift;                           \
        if (!BDDAndersPtsSet::parseReorderingMethod(methodName, method)) {     \
          llvm::report_fatal_error(                                            \
              llvm::Twine("Unknown BDD reordering method: ") + methodName);    \
        }                                                                      \
        BDDAndersPtsSet::configureReordering(true, method);                    \
      } else {                                                                 \
        BDDAndersPtsSet::configureReordering(false);                           \
      }                                                                        \
      std::cout << "Using BDD-based points-to analysis\n";                     \
      using Pts = BDDPts;                                                      \
      block;                                                                   \
    } else {                                                                   \
      std::cout << "Using BitVector-based points-to analysis\n";               \
      using Pts = BitVectorPTS;                                                \
      block;                                                                   \
    }                                                                          \
  }

int main(int argc, char **argv) {
  // Parse command line
  cl::ParseCommandLineOptions(
      argc, argv, "AserPTA - High-Performance Pointer Analysis Tool\n");

  // Load IR module
  SMDiagnostic Err;
  LLVMContext Context;
  auto module = loadIRModule(InputFilename, Context, Err, argv[0]);

  if (!module) {
    return 1;
  }

  errs() << "Loaded module: " << InputFilename << "\n";
  errs() << "Analysis mode: " << AnalysisMode << "\n";
  errs() << "Solver type: " << SolverType << "\n";
  errs() << "Field-sensitive: " << (FieldSensitive ? "yes" : "no") << "\n";

  AnalysisOptions analysisOptions;
  analysisOptions.readPointsToPath = ReadPointsTo;
  analysisOptions.writePointsToPath = WritePointsTo;

  // Initialize AliasSpecManager with config files
  auto specFilePaths = collectConfigFilePaths();
  auto specManager = createAliasSpecManager(specFilePaths, module.get());

  // Display loaded config files
  printLoadedConfigFiles(*specManager);

  // Setup origin rules for origin-sensitive analysis
  Origin::setOriginRules(
      [](const Origin *, const llvm::Instruction *I) -> bool {
        if (auto *CB = llvm::dyn_cast<CallBase>(I)) {
          if (auto *F = CB->getCalledFunction()) {
            StringRef name = F->getName();
            // Track thread creation and spawn operations as origins
            return name.equals("pthread_create") || name.contains("spawn") ||
                   name.contains("thread");
          }
        }
        return false;
      });

  // Run analysis based on mode and solver

  determinePts(
      try {
        if (AnalysisMode == "ci") {
          // Context-insensitive
          if (SolverType == "basic") {
            runAnalysis<CIBasicSolver<Pts>>(*module, analysisOptions,
                                            DumpStats);
          } else if (SolverType == "wave") {
            runAnalysis<CIWaveSolver<Pts>>(*module, analysisOptions,
                                           DumpStats);
          } else if (SolverType == "deep") {
            runAnalysis<CIDeepSolver<Pts>>(*module, analysisOptions,
                                           DumpStats);
          } else {
            errs() << "Unknown solver type: " << SolverType << "\n";
            return 1;
          }
        } else if (AnalysisMode == "1-cfa") {
          // 1-call-site sensitive
          if (SolverType == "basic") {
            runAnalysis<CS1BasicSolver<Pts>>(*module, analysisOptions,
                                             DumpStats);
          } else if (SolverType == "wave") {
            runAnalysis<CS1WaveSolver<Pts>>(*module, analysisOptions,
                                            DumpStats);
          } else if (SolverType == "deep") {
            runAnalysis<CS1DeepSolver<Pts>>(*module, analysisOptions,
                                            DumpStats);
          } else {
            errs() << "Unknown solver type: " << SolverType << "\n";
            return 1;
          }
        } else if (AnalysisMode == "2-cfa") {
          // 2-call-site sensitive
          if (SolverType == "basic") {
            runAnalysis<CS2BasicSolver<Pts>>(*module, analysisOptions,
                                             DumpStats);
          } else if (SolverType == "wave") {
            runAnalysis<CS2WaveSolver<Pts>>(*module, analysisOptions,
                                            DumpStats);
          } else if (SolverType == "deep") {
            runAnalysis<CS2DeepSolver<Pts>>(*module, analysisOptions,
                                            DumpStats);
          } else {
            errs() << "Unknown solver type: " << SolverType << "\n";
            return 1;
          }
        } else if (AnalysisMode == "origin") {
          // Origin-sensitive
          if (SolverType == "basic") {
            runAnalysis<OriginBasicSolver<Pts>>(*module, analysisOptions,
                                                DumpStats);
          } else if (SolverType == "wave") {
            runAnalysis<OriginWaveSolver<Pts>>(*module, analysisOptions,
                                               DumpStats);
          } else if (SolverType == "deep") {
            runAnalysis<OriginDeepSolver<Pts>>(*module, analysisOptions,
                                               DumpStats);
          } else {
            errs() << "Unknown solver type: " << SolverType << "\n";
            return 1;
          }
        } else {
          errs() << "Unknown analysis mode: " << AnalysisMode << "\n";
          errs() << "Valid modes: ci, 1-cfa, 2-cfa, origin\n";
          return 1;
        }
      } catch (const std::exception &e) {
        errs() << "Error during analysis: " << e.what() << "\n";
        return 1;
      });

  return 0;
}
