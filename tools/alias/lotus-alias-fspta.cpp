/**
 * @file lotus-alias-fspta.cpp
 * @brief Driver for Lotus flow-sensitive pointer analyses.
 */
#include "Alias/InclusionBased/FlowSensitive/Sparse/FlowSensitivePTA.h"
#include "Alias/InclusionBased/FlowSensitive/Support/PointerOnlyView.h"
#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowPTA.h"
#include "Alias/InclusionBased/FlowSensitive/Versioned/VersionedFlowSensitivePTA.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"

#include <algorithm>
#include <exception>
#include <map>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias;
using namespace lotus::alias::tools;
using namespace lotus::analysis;

namespace {
enum class SetBackendOption { Mutable, HashConsed };
enum class PartitionOption { Distinct, IntraDisjoint, InterDisjoint };
enum class AnalysisOption { FlowSensitive, Versioned, ValueFlow };
enum class ParallelOrderOption { Reference, Unordered };
enum class ClusterOption { None, Type, Footprint };
enum class PrelabelOption { Builder, PreAnalysis };

cl::OptionCategory FsptaCategory("Lotus flow-sensitive pointer analyses");
cl::opt<bool>
    Parallel("parallel",
             cl::desc("Pipeline fspta transfers with dependency validation"),
             cl::init(false), cl::cat(FsptaCategory));
cl::opt<ParallelOrderOption> ParallelOrder(
    "parallel-order", cl::desc("Parallel commit policy"),
    cl::values(
        clEnumValN(ParallelOrderOption::Reference, "reference",
                   "Preserve sequential transfer order (default)"),
        clEnumValN(ParallelOrderOption::Unordered, "unordered",
                   "Research ablation: may select a different fixed point")),
    cl::init(ParallelOrderOption::Reference), cl::cat(FsptaCategory));
cl::opt<unsigned> Threads(
    "threads",
    cl::desc("Parallel workers including caller (0: hardware concurrency)"),
    cl::init(0), cl::cat(FsptaCategory));
cl::opt<unsigned> VersioningThreads(
    "versioning-threads",
    cl::desc("Threads computing vfspta version labels (0: hardware "
             "concurrency, 1: sequential)"),
    cl::init(0), cl::cat(FsptaCategory));
cl::opt<bool> VerifyParallel(
    "verify-parallel",
    cl::desc("Compare every fspta result with sequential solving"),
    cl::init(false), cl::cat(FsptaCategory));
cl::opt<bool>
    ParallelShareSets("parallel-share-sets",
                      cl::desc("Share immutable parallel effect sets"),
                      cl::init(true), cl::cat(FsptaCategory));
cl::opt<unsigned>
    ParallelBlockSize("parallel-block-size",
                      cl::desc("Transfers per speculative worker block"),
                      cl::init(8), cl::cat(FsptaCategory));
cl::opt<bool>
    ParallelMemo("parallel-memo",
                 cl::desc("Reuse certified unchanged transfer effects"),
                 cl::init(true), cl::cat(FsptaCategory));
cl::opt<bool> ParallelObjectCertificates(
    "parallel-object-certificates",
    cl::desc("Validate precise memory reads by object"), cl::init(true),
    cl::cat(FsptaCategory));
cl::opt<std::string> InputFilename(cl::Positional,
                                   cl::desc("<input bitcode file>"),
                                   cl::Required, cl::cat(FsptaCategory));
cl::opt<AnalysisOption> Analysis(
    "analysis", cl::desc("Flow-sensitive analysis implementation"),
    cl::values(clEnumValN(AnalysisOption::FlowSensitive, "fspta",
                          "Conventional sparse flow-sensitive analysis"),
               clEnumValN(AnalysisOption::Versioned, "vfspta",
                          "Object-versioned flow-sensitive analysis"),
               clEnumValN(AnalysisOption::ValueFlow, "vfpta",
                          "Value-flow-based flow-sensitive analysis")),
    cl::init(AnalysisOption::FlowSensitive), cl::cat(FsptaCategory));
cl::opt<SetBackendOption> SetBackend(
    "points-to-sets", cl::desc("Points-to set backend"),
    cl::values(clEnumValN(SetBackendOption::Mutable, "mutable",
                          "Mutable ordered sets (default)"),
               clEnumValN(SetBackendOption::HashConsed, "hash-consed",
                          "Interned immutable sets with operation caching")),
    cl::init(SetBackendOption::Mutable), cl::cat(FsptaCategory));
cl::opt<PartitionOption> MemoryPartition(
    "memory-partition", cl::desc("MemorySSA region partition"),
    cl::values(clEnumValN(PartitionOption::Distinct, "distinct",
                          "Exact points-to regions"),
               clEnumValN(PartitionOption::IntraDisjoint, "intra-disjoint",
                          "Disjoint regions per function"),
               clEnumValN(PartitionOption::InterDisjoint, "inter-disjoint",
                          "Disjoint whole-program regions")),
    cl::init(PartitionOption::InterDisjoint), cl::cat(FsptaCategory));
cl::opt<bool> PrintPointsTo("print-pts",
                            cl::desc("Print top-level points-to results"),
                            cl::init(false), cl::cat(FsptaCategory));
cl::opt<bool> PrintMemory("print-memory",
                          cl::desc("Print non-empty sparse memory facts"),
                          cl::init(false), cl::cat(FsptaCategory));
cl::opt<bool> DumpStats("dump-stats", cl::desc("Print solver statistics"),
                        cl::init(true), cl::cat(FsptaCategory));
cl::opt<double> TimeLimit(
    "time-limit",
    cl::desc("Wall-clock budget for the flow-sensitive solve in seconds "
             "(0 disables the limit)"),
    cl::init(0.0), cl::cat(FsptaCategory));
cl::opt<bool> StatsReport(
    "stats-report",
    cl::desc("Print the solver's full statistics report (SVF PrintStat-like)"),
    cl::init(false), cl::cat(FsptaCategory));
cl::opt<std::string> ReadPtsFile(
    "read-pts",
    cl::desc("Read solved points-to facts from a Lotus result file instead of "
             "solving"),
    cl::init(""), cl::cat(FsptaCategory));
cl::opt<std::string> WritePtsFile(
    "write-pts",
    cl::desc("Write solved points-to facts to a Lotus result file after "
             "solving"),
    cl::init(""), cl::cat(FsptaCategory));
cl::opt<std::string>
    DumpSVFG("dump-svfg", cl::desc("Write the initialized SVFG as a DOT file"),
             cl::init(""), cl::cat(FsptaCategory));
cl::opt<bool> ValidateAnnotations(
    "validate-annotations",
    cl::desc("Validate __aser_alias__/__aser_no_alias__ calls"),
    cl::init(false), cl::cat(FsptaCategory));
cl::opt<ClusterOption> ClusterObjects(
    "cluster-objects", cl::desc("Object clustering applied before solving"),
    cl::values(
        clEnumValN(ClusterOption::None, "none",
                   "Identity mapping (SVF's plainMap; default)"),
        clEnumValN(ClusterOption::Type, "type",
                   "Merge base objects with the same LLVM type and kind"),
        clEnumValN(ClusterOption::Footprint, "footprint",
                   "Merge objects with identical access footprints "
                   "(vfspta only)")),
    cl::init(ClusterOption::None), cl::cat(FsptaCategory));
cl::opt<bool> PointerOnly(
    "pointer-only",
    cl::desc("Model only pointer-typed loads/stores, like SVF's pointer-only "
             "SVFG (default: enabled)"),
    cl::init(true), cl::cat(FsptaCategory));
cl::opt<bool> VfsptaIncrementalRecursion(
    "vfspta-incremental-recursion",
    cl::desc("Demote strong updates to weak updates incrementally when an "
             "indirect call creates recursion, avoiding a whole-graph relabel "
             "(default: disabled)"),
    cl::init(false), cl::cat(FsptaCategory));
cl::opt<PrelabelOption> VfsptaPrelabel(
    "vfspta-prelabel", cl::desc("Seed used for vfspta meld labels"),
    cl::values(
        clEnumValN(PrelabelOption::Builder, "builder",
                   "MemorySSA access regions recorded by the SVFG builder "
                   "(default)"),
        clEnumValN(PrelabelOption::PreAnalysis, "pre-analysis",
                   "Points-to sets from the pre-analysis (SVF's prelabel())")),
    cl::init(PrelabelOption::Builder), cl::cat(FsptaCategory));

MemoryRegionPartitionStrategy partitionStrategy() {
  switch (MemoryPartition) {
  case PartitionOption::Distinct:
    return MemoryRegionPartitionStrategy::Distinct;
  case PartitionOption::IntraDisjoint:
    return MemoryRegionPartitionStrategy::IntraDisjoint;
  case PartitionOption::InterDisjoint:
    return MemoryRegionPartitionStrategy::InterDisjoint;
  }
  llvm_unreachable("invalid memory partition");
}

void printValue(const Value *value, raw_ostream &os) {
  if (!value)
    os << "<unknown>";
  else if (const auto *instruction = dyn_cast<Instruction>(value)) {
    if (instruction->getFunction())
      os << instruction->getFunction()->getName() << "::";
    if (value->hasName())
      os << value->getName();
    else
      value->printAsOperand(os, false);
  } else if (value->hasName())
    os << value->getName();
  else
    value->printAsOperand(os, false);
}

int runValueFlowAnalysis(const Module &module) {
  if (PrintMemory) {
    errs() << "lotus-alias-fspta: --print-memory is unavailable for vfpta; "
              "use --print-pts instead\n";
    return 1;
  }
  if (!DumpSVFG.empty()) {
    errs() << "lotus-alias-fspta: --dump-svfg is unavailable for vfpta, "
              "which builds its own value-flow graph\n";
    return 1;
  }

  ValueFlowPTA solver(module);
  try {
    solver.analyze();
  } catch (const std::exception &error) {
    errs() << "lotus-alias-fspta: vfpta failed: " << error.what() << "\n";
    return 1;
  }

  if (PrintPointsTo) {
    auto printPointsTo = [&](const Value *value) {
      if (!value || !value->getType()->isPointerTy())
        return;
      outs() << "pts(";
      printValue(value, outs());
      outs() << ") = {";
      bool first = true;
      for (ValueFlowPTA::ObjectID object : solver.getPointsTo(value)) {
        if (!first)
          outs() << ", ";
        first = false;
        outs() << object << ":";
        if (object == solver.getNullObjectId())
          outs() << "<null>";
        else if (object == solver.getUnknownObjectId())
          outs() << "<unknown>";
        else
          printValue(solver.getObjectValue(object), outs());
      }
      outs() << "}\n";
    };
    for (const GlobalVariable &global : module.globals())
      printPointsTo(&global);
    for (const Function &function : module) {
      printPointsTo(&function);
      for (const Argument &argument : function.args())
        printPointsTo(&argument);
      if (!function.isDeclaration())
        for (const Instruction &instruction : instructions(function))
          printPointsTo(&instruction);
    }
  }

  if (DumpStats) {
    const auto &stats = solver.getStatistics();
    outs() << "vfpta.objects=" << stats.objects << "\n"
           << "vfpta.value-nodes=" << stats.valueNodes << "\n"
           << "vfpta.control-blocks=" << stats.controlBlocks << "\n"
           << "vfpta.call-graph-iterations=" << stats.callGraphIterations
           << "\n"
           << "vfpta.resolved-indirect-targets="
           << stats.resolvedIndirectTargets << "\n"
           << "vfpta.conservative-external-calls="
           << stats.conservativeExternalCalls << "\n"
           << "vfpta.object-traversals=" << stats.solver.objectTraversals
           << "\n"
           << "vfpta.indirect-flow-computations="
           << stats.solver.indirectFlowComputations << "\n"
           << "vfpta.indirect-edges=" << stats.solver.indirectEdges << "\n"
           << "vfpta.strong-update-sites=" << stats.solver.strongUpdateSites
           << "\n"
           << "vfpta.projected-events=" << stats.solver.projectedEvents << "\n"
           << "vfpta.expanded-control-flow-nodes="
           << stats.solver.expandedControlFlowNodes << "\n"
           << "vfpta.idf-nodes=" << stats.solver.iteratedDominanceFrontierNodes
           << "\n"
           << "vfpta.sparse-graph-nodes=" << stats.solver.sparseGraphNodes
           << "\n"
           << "vfpta.sparse-graph-edges=" << stats.solver.sparseGraphEdges
           << "\n"
           << "vfpta.used-weak-fallback="
           << (stats.solver.usedWeakFallback ? "true" : "false") << "\n";
    if (stats.solver.usedWeakFallback)
      outs() << "vfpta.weak-fallback-reason=" << stats.solver.fallbackReason
             << "\n";
  }

  std::size_t validations = 0;
  std::size_t failures = 0;
  if (ValidateAnnotations) {
    for (const Function &function : module) {
      for (const Instruction &instruction : instructions(function)) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        const Function *callee = call ? call->getCalledFunction() : nullptr;
        if (!callee || call->arg_size() < 2)
          continue;
        const bool expectAlias = callee->getName() == "__aser_alias__";
        const bool expectNoAlias = callee->getName() == "__aser_no_alias__";
        if (!expectAlias && !expectNoAlias)
          continue;
        ++validations;
        const bool result =
            solver.mayAlias(call->getArgOperand(0), call->getArgOperand(1));
        if (result == expectAlias)
          continue;
        ++failures;
        errs() << InputFilename << ": annotation mismatch in "
               << function.getName() << ": expected "
               << (expectAlias ? "alias" : "no-alias") << ", got "
               << (result ? "alias" : "no-alias") << " for ";
        printValue(call->getArgOperand(0), errs());
        errs() << " and ";
        printValue(call->getArgOperand(1), errs());
        errs() << "\n";
      }
    }
    outs() << "vfpta.validations=" << validations << "\n"
           << "vfpta.validation-failures=" << failures << "\n";
  }
  return failures == 0 ? 0 : 2;
}
} // namespace

int main(int argc, char **argv) {
  InitLLVM init(argc, argv);
  cl::HideUnrelatedOptions(FsptaCategory);
  cl::ParseCommandLineOptions(argc, argv,
                              "Lotus flow-sensitive pointer analyses\n");
  if ((Parallel || VerifyParallel || Threads != 0 ||
       ParallelOrder.getNumOccurrences() != 0 ||
       ParallelShareSets.getNumOccurrences() != 0 ||
       ParallelBlockSize.getNumOccurrences() != 0 ||
       ParallelMemo.getNumOccurrences() != 0 ||
       ParallelObjectCertificates.getNumOccurrences() != 0) &&
      Analysis != AnalysisOption::FlowSensitive) {
    errs() << "Parallel execution is supported only for --analysis=fspta\n";
    return 1;
  }
  if ((VerifyParallel || Threads != 0 ||
       ParallelOrder.getNumOccurrences() != 0 ||
       ParallelShareSets.getNumOccurrences() != 0 ||
       ParallelBlockSize.getNumOccurrences() != 0 ||
       ParallelMemo.getNumOccurrences() != 0 ||
       ParallelObjectCertificates.getNumOccurrences() != 0) &&
      !Parallel) {
    errs() << "Parallel execution options require --parallel\n";
    return 1;
  }
  if (ParallelBlockSize == 0) {
    errs() << "--parallel-block-size must be positive\n";
    return 1;
  }
  if (!ReadPtsFile.empty() && VerifyParallel) {
    errs() << "--verify-parallel cannot be combined with --read-pts\n";
    return 1;
  }
  if ((!ReadPtsFile.empty() || !WritePtsFile.empty()) &&
      Analysis == AnalysisOption::ValueFlow) {
    errs() << "--read-pts/--write-pts support --analysis=fspta and vfspta "
              "only\n";
    return 1;
  }
  if (ClusterObjects == ClusterOption::Footprint &&
      Analysis != AnalysisOption::Versioned) {
    errs() << "--cluster-objects=footprint is supported only for "
              "--analysis=vfspta\n";
    return 1;
  }
  if (VfsptaPrelabel.getNumOccurrences() != 0 &&
      Analysis != AnalysisOption::Versioned) {
    errs() << "--vfspta-prelabel is supported only for --analysis=vfspta\n";
    return 1;
  }
  if (Analysis == AnalysisOption::ValueFlow &&
      (ClusterObjects.getNumOccurrences() != 0 ||
       PointerOnly.getNumOccurrences() != 0)) {
    errs() << "--cluster-objects/--pointer-only are unavailable for vfpta\n";
    return 1;
  }
  LLVMContext context;
  SMDiagnostic diagnostic;
  std::unique_ptr<Module> module =
      loadIRModule(InputFilename, context, diagnostic, argv[0]);
  if (!module)
    return 1;

  if (Analysis == AnalysisOption::ValueFlow)
    return runValueFlowAnalysis(*module);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  graphConfig.memoryPartition = partitionStrategy();
  auto graphBuilder = std::make_unique<SVFGBuilder>(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder->build(&icfg));
  graphBuilder->connectPreAnalysisIndirectCalls(graph.get());
  if (!DumpSVFG.empty())
    graph->dump(DumpSVFG);

  auto isAllowedIndirectCall = [&](const CallBase *callSite,
                                   const Function *target) {
    const std::vector<const Function *> allowed =
        graphBuilder->getIndirectCallTargets(callSite);
    return std::find(allowed.begin(), allowed.end(), target) != allowed.end();
  };
  auto connectIndirectCall = [&](const CallBase *callSite,
                                 const Function *target) {
    if (!isAllowedIndirectCall(callSite, target))
      return false;
    std::vector<SVFGEdge *> edges;
    return graphBuilder->connectCallSiteToCalleeOnTheFly(graph.get(), callSite,
                                                         target, edges);
  };
  std::unique_ptr<lotus::analysis::FilteredSVFGView> pointerOnlyView;
  if (PointerOnly)
    pointerOnlyView =
        lotus::alias::flow_sensitive::createPointerOnlySVFGView(*graph);

  std::unique_ptr<FlowSensitivePTA> solver;
  std::unique_ptr<VersionedFlowSensitivePTA> versionedSolver;
  if (Analysis == AnalysisOption::Versioned) {
    VersionedFlowSensitivePTA::Config solverConfig;
    if (pointerOnlyView)
      solverConfig.scope = pointerOnlyView.get();
    solverConfig.connectIndirectCall = connectIndirectCall;
    solverConfig.isIndirectCallTargetAllowed = isAllowedIndirectCall;
    solverConfig.versioningThreads = VersioningThreads;
    solverConfig.timeLimitSeconds = TimeLimit;
    solverConfig.pointerOnlySVFG = PointerOnly;
    solverConfig.incrementalRecursionUpdate = VfsptaIncrementalRecursion;
    switch (ClusterObjects) {
    case ClusterOption::None:
      solverConfig.clusterObjects =
          VersionedFlowSensitivePTA::Config::ClusterMethod::None;
      break;
    case ClusterOption::Type:
      solverConfig.clusterObjects =
          VersionedFlowSensitivePTA::Config::ClusterMethod::Type;
      break;
    case ClusterOption::Footprint:
      solverConfig.clusterObjects =
          VersionedFlowSensitivePTA::Config::ClusterMethod::Footprint;
      break;
    }
    if (VfsptaPrelabel == PrelabelOption::PreAnalysis) {
      solverConfig.preAnalysisTargets = [&graphBuilder](const Value *value) {
        return graphBuilder->getPreAnalysisPointsTo(value);
      };
    }
    versionedSolver = std::make_unique<VersionedFlowSensitivePTA>(
        *graph, std::move(solverConfig));
    if (!ReadPtsFile.empty()) {
      if (!versionedSolver->readVersionedAnalysisResultFromFile(ReadPtsFile)) {
        errs() << "failed to read points-to results from '" << ReadPtsFile
               << "'\n";
        return 1;
      }
    } else {
      versionedSolver->solve();
    }
    if (!WritePtsFile.empty() &&
        !versionedSolver->writeVersionedAnalysisResultToFile(WritePtsFile)) {
      errs() << "failed to write points-to results to '" << WritePtsFile
             << "'\n";
      return 1;
    }
  } else {
    FlowSensitivePTA::Config solverConfig;
    if (pointerOnlyView)
      solverConfig.scope = pointerOnlyView.get();
    solverConfig.parallel = Parallel;
    solverConfig.unorderedParallel =
        ParallelOrder == ParallelOrderOption::Unordered;
    solverConfig.workerThreads = Threads;
    solverConfig.shareParallelSets = ParallelShareSets;
    solverConfig.parallelBlockSize = ParallelBlockSize;
    solverConfig.parallelMemoize = ParallelMemo;
    solverConfig.parallelObjectCertificates = ParallelObjectCertificates;
    solverConfig.setBackend = SetBackend == SetBackendOption::HashConsed
                                  ? PointsToSetBackend::HashConsed
                                  : PointsToSetBackend::Mutable;
    solverConfig.connectIndirectCall = connectIndirectCall;
    solverConfig.isIndirectCallTargetAllowed = isAllowedIndirectCall;
    solverConfig.timeLimitSeconds = TimeLimit;
    solverConfig.pointerOnlySVFG = PointerOnly;
    if (ClusterObjects == ClusterOption::Type)
      solverConfig.clusterObjects =
          FlowSensitivePTA::Config::ClusterMethod::Type;
    solver =
        std::make_unique<FlowSensitivePTA>(*graph, std::move(solverConfig));
    if (!ReadPtsFile.empty()) {
      if (!solver->readAnalysisResultFromFile(ReadPtsFile)) {
        errs() << "failed to read points-to results from '" << ReadPtsFile
               << "'\n";
        return 1;
      }
    } else {
      solver->solve();
    }
    if (!WritePtsFile.empty() &&
        !solver->writeAnalysisResultToFile(WritePtsFile)) {
      errs() << "failed to write points-to results to '" << WritePtsFile
             << "'\n";
      return 1;
    }
    if (VerifyParallel && ReadPtsFile.empty()) {
      // Both solvers see the final graph after all parallel topology epochs.
      // No additional graph mutation is permitted during this comparison.
      FlowSensitivePTA reference(*graph);
      reference.solve();
      std::string difference;
      const bool equal = solver->hasSameSolution(reference, &difference);
      outs() << "fspta.parallel-equivalent=" << (equal ? "true" : "false")
             << "\n";
      if (!equal) {
        errs() << "Parallel/sequential flow-sensitive results differ: "
               << difference << "\n";
        std::string parallelDifference, sequentialDifference;
        errs() << "Parallel fixed point: "
               << solver->isFixedPoint(&parallelDifference) << " "
               << parallelDifference << "\nSequential fixed point: "
               << reference.isFixedPoint(&sequentialDifference) << " "
               << sequentialDifference << "\n";
        return 3;
      }
    }
  }
  auto queryPointsTo = [&](const Value *value) -> std::optional<SVFGNodeBS> {
    return versionedSolver ? versionedSolver->pointsTo(value)
                           : solver->pointsTo(value);
  };
  auto queryMemory = [&](const SVFGNode *node, uint32_t object, bool outgoing) {
    if (!versionedSolver)
      return outgoing ? SVFGNodeBS(solver->memoryOut(node, object))
                      : SVFGNodeBS(solver->memoryIn(node, object));
    const auto version =
        outgoing ? versionedSolver->getYield(node->getId(), object)
                 : versionedSolver->getConsume(node->getId(), object);
    return SVFGNodeBS(versionedSolver->versionedPointsTo(object, version));
  };

  if (PrintPointsTo) {
    for (const auto &[value, nodeID] : graph->getValueNodeMap()) {
      if (!value || !value->getType()->isPointerTy())
        continue;
      const auto result = queryPointsTo(value);
      if (!result)
        continue;
      outs() << "pts(";
      printValue(value, outs());
      outs() << ") = {";
      bool first = true;
      for (uint32_t object : *result) {
        if (!first)
          outs() << ", ";
        first = false;
        outs() << object << ":";
        printValue(graph->getObjectValue(object), outs());
      }
      outs() << "}\n";
    }
  }
  if (PrintMemory) {
    for (const auto &[nodeID, node] : *graph) {
      bool printedNode = false;
      std::set<uint32_t> flowTargets;
      std::map<uint32_t, std::string> objects(
          graph->getObjectDebugMap().begin(), graph->getObjectDebugMap().end());
      if (const auto *instruction =
              dyn_cast_or_null<Instruction>(node->getValue())) {
        const Value *pointer = nullptr;
        if (const auto *load = dyn_cast<LoadInst>(instruction))
          pointer = load->getPointerOperand();
        else if (const auto *store = dyn_cast<StoreInst>(instruction))
          pointer = store->getPointerOperand();
        if (pointer)
          if (auto targets = queryPointsTo(pointer))
            for (uint32_t object : *targets)
              flowTargets.insert(object);
      }
      for (const auto &[object, label] : objects) {
        const SVFGNodeBS in = queryMemory(node, object, false);
        const SVFGNodeBS out = queryMemory(node, object, true);
        if (in.empty() && out.empty() && flowTargets.count(object) == 0)
          continue;
        if (!printedNode) {
          outs() << "memory(N" << nodeID
                 << ",kind=" << static_cast<unsigned>(node->getNodeKind());
          if (node->getValue()) {
            outs() << ",value=";
            if (const auto *instruction =
                    dyn_cast<Instruction>(node->getValue()))
              instruction->print(outs());
            else
              printValue(node->getValue(), outs());
          }
          outs() << ")\n";
          printedNode = true;
        }
        outs() << "  obj " << object << " [" << label
               << (graph->isConstantObject(object) ? ",constant" : "")
               << ",base="
               << (graph->getObjectInfo(object)
                       ? graph->getObjectInfo(object)->baseObjId
                       : 0)
               << "] in={";
        bool first = true;
        for (uint32_t target : in) {
          if (!first)
            outs() << ",";
          first = false;
          outs() << target;
        }
        outs() << "} out={";
        first = true;
        for (uint32_t target : out) {
          if (!first)
            outs() << ",";
          first = false;
          outs() << target;
        }
        outs() << "}\n";
      }
    }
  }
  if (StatsReport) {
    outs() << (versionedSolver ? versionedSolver->statisticsReport()
                               : solver->statisticsReport());
    if (versionedSolver ? versionedSolver->timedOut() : solver->timedOut())
      outs() << "warning: solve timed out; results are a partial fixed point\n";
  }
  if (DumpStats) {
    if (versionedSolver) {
      const auto &stats = versionedSolver->statistics();
      outs() << "vfspta.nodes=" << stats.nodes << "\n"
             << "vfspta.node-processes=" << stats.nodeProcesses << "\n"
             << "vfspta.versioned-objects=" << stats.versionedObjects << "\n"
             << "vfspta.equivalent-objects=" << stats.equivalentObjects << "\n"
             << "vfspta.versions=" << stats.versions << "\n"
             << "vfspta.versioned-facts=" << stats.versionedFacts << "\n"
             << "vfspta.version-propagations=" << stats.versionPropagations
             << "\n"
             << "vfspta.statement-reliances=" << stats.statementReliances
             << "\n"
             << "vfspta.strong-updates=" << stats.strongUpdates << "\n"
             << "vfspta.weak-updates=" << stats.weakUpdates << "\n"
             << "vfspta.indirect-call-edges=" << stats.indirectCallEdges << "\n"
             << "vfspta.delta-nodes=" << stats.deltaNodes << "\n"
             << "vfspta.delta-source-nodes=" << stats.deltaSourceNodes << "\n"
             << "vfspta.delta-version-updates=" << stats.deltaVersionUpdates
             << "\n"
             << "vfspta.delta-relabelings=" << stats.deltaRelabelings << "\n"
             << "vfspta.statement-worklist-pushes="
             << stats.statementWorklistPushes << "\n"
             << "vfspta.relabelings=" << stats.relabelings << "\n"
             << "vfspta.clustered-objects=" << stats.clusteredObjects << "\n"
             << "vfspta.merged-object-clusters=" << stats.mergedObjectClusters
             << "\n";
    } else {
      const auto &stats = solver->statistics();
      outs() << "fspta.nodes=" << stats.nodes << "\n"
             << "fspta.sccs=" << stats.sccs << "\n"
             << "fspta.max-scc-size=" << stats.maxSccSize << "\n"
             << "fspta.node-processes=" << stats.nodeProcesses << "\n"
             << "fspta.top-level-facts=" << stats.topLevelFacts << "\n"
             << "fspta.memory-in-facts=" << stats.memoryInFacts << "\n"
             << "fspta.memory-out-facts=" << stats.memoryOutFacts << "\n"
             << "fspta.strong-updates=" << stats.strongUpdates << "\n"
             << "fspta.weak-updates=" << stats.weakUpdates << "\n"
             << "fspta.strong-update-executions="
             << stats.strongUpdateExecutions << "\n"
             << "fspta.weak-update-executions=" << stats.weakUpdateExecutions
             << "\n"
             << "fspta.indirect-call-edges=" << stats.indirectCallEdges << "\n"
             << "fspta.hash-consed-sets=" << stats.hashConsedUniqueSets << "\n"
             << "fspta.clustered-objects=" << stats.clusteredObjects << "\n"
             << "fspta.merged-object-clusters=" << stats.mergedObjectClusters
             << "\n";
      outs() << "fspta.solve-seconds=" << stats.solveSeconds << "\n"
             << "fspta.parallel-order="
             << (!Parallel ? "sequential"
                 : ParallelOrder == ParallelOrderOption::Reference
                     ? "reference"
                     : "unordered")
             << "\n"
             << "fspta.parallel-threads=" << stats.parallelThreads << "\n"
             << "fspta.parallel-commits=" << stats.parallelCommits << "\n"
             << "fspta.parallel-conflicts=" << stats.parallelConflicts << "\n"
             << "fspta.parallel-publications=" << stats.parallelPublications
             << "\n"
             << "fspta.parallel-notifications=" << stats.parallelNotifications
             << "\n"
             << "fspta.parallel-peak-workers=" << stats.parallelPeakWorkers
             << "\n"
             << "fspta.parallel-blocks=" << stats.parallelBlocks << "\n"
             << "fspta.parallel-forwarded-reads="
             << stats.parallelForwardedReads << "\n"
             << "fspta.parallel-accepted-forwarded-reads="
             << stats.parallelAcceptedForwardedReads << "\n"
             << "fspta.parallel-memo-hits=" << stats.parallelMemoHits << "\n"
             << "fspta.parallel-object-reads=" << stats.parallelObjectReads
             << "\n"
             << "fspta.parallel-negative-reads=" << stats.parallelNegativeReads
             << "\n"
             << "fspta.parallel-wildcard-reads=" << stats.parallelWildcardReads
             << "\n"
             << "fspta.parallel-object-salvages="
             << stats.parallelObjectSalvages << "\n"
             << "fspta.topology-epochs=" << stats.topologyEpochs << "\n";
    }
  }
  std::size_t validations = 0;
  std::size_t failures = 0;
  if (ValidateAnnotations) {
    for (const Function &function : *module) {
      for (const Instruction &instruction : instructions(function)) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        const Function *callee = call ? call->getCalledFunction() : nullptr;
        if (!callee || call->arg_size() < 2)
          continue;
        const bool expectAlias = callee->getName() == "__aser_alias__";
        const bool expectNoAlias = callee->getName() == "__aser_no_alias__";
        if (!expectAlias && !expectNoAlias)
          continue;
        ++validations;
        const auto result =
            versionedSolver ? versionedSolver->mayAlias(call->getArgOperand(0),
                                                        call->getArgOperand(1))
                            : solver->mayAlias(call->getArgOperand(0),
                                               call->getArgOperand(1));
        if (!result || *result != expectAlias) {
          ++failures;
          errs() << InputFilename << ": annotation mismatch in "
                 << function.getName() << ": expected "
                 << (expectAlias ? "alias" : "no-alias") << ", got ";
          if (!result)
            errs() << "unresolved";
          else
            errs() << (*result ? "alias" : "no-alias");
          errs() << " for ";
          printValue(call->getArgOperand(0), errs());
          errs() << " and ";
          printValue(call->getArgOperand(1), errs());
          errs() << "\n";
        }
      }
    }
    const StringRef prefix = versionedSolver ? "vfspta" : "fspta";
    outs() << prefix << ".validations=" << validations << "\n"
           << prefix << ".validation-failures=" << failures << "\n";
  }
  return failures == 0 ? 0 : 2;
}
