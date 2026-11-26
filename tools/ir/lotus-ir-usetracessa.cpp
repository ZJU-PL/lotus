#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/MemorySSADump.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/UseTraceSSA/DefectDetector.h"
#include "IR/UseTraceSSA/Query.h"
#include "IR/UseTraceSSA/SVFGBridge.h"
#include "IR/UseTraceSSA/TraceQueryOptions.h"
#include "Utils/Graph/DotWriter.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>

using namespace lotus::analysis;
using namespace lotus::usetracessa;

namespace {
llvm::cl::opt<std::string> Input(llvm::cl::Positional,
                                  llvm::cl::desc("<input.ll or input.bc>"),
                                  llvm::cl::Required);
llvm::cl::opt<std::string> Format("format", llvm::cl::desc("text, json, or dot"),
                                   llvm::cl::init("text"));
llvm::cl::opt<std::string> DumpSVFG("dump-svfg",
                                    llvm::cl::desc("Write the source SVFG DOT to a file"),
                                    llvm::cl::init(""));
llvm::cl::opt<std::string> DumpSVFGFunction(
    "dump-svfg-function",
    llvm::cl::desc("Restrict --dump-svfg to one function (e.g. main)"),
    llvm::cl::init(""));
llvm::cl::opt<bool> DumpMSSA(
    "dump-mssa", llvm::cl::desc("Dump sparse MemorySSA MU/CHI/PHI text"));
llvm::cl::opt<std::string> DumpMSSAFunction(
    "dump-mssa-function",
    llvm::cl::desc("Restrict --dump-mssa to one function (e.g. main)"),
    llvm::cl::init(""));
llvm::cl::opt<std::string> Check("check",
                                 llvm::cl::desc(
                                     "double-free, use-after-free, memory-leak, or file-leak"),
                                 llvm::cl::init(""));
llvm::cl::opt<bool> Timing("timing",
                            llvm::cl::desc("Print analysis phase timings to stderr"));
llvm::cl::opt<bool> Quiet("quiet",
                           llvm::cl::desc("Suppress issue and witness details"));
llvm::cl::opt<bool> FullLabels(
    "full-labels",
    llvm::cl::desc("Capture exact LLVM instruction labels even with --quiet"));
cli::QueryOptionsParser QueryOptionFlags;
llvm::cl::opt<unsigned> Source("source-node",
                                llvm::cl::desc("SVFG source node ID for a flow query"),
                                llvm::cl::init(std::numeric_limits<unsigned>::max()));
llvm::cl::opt<unsigned> Sink("sink-node",
                              llvm::cl::desc("SVFG sink node ID for a flow query"),
                              llvm::cl::init(std::numeric_limits<unsigned>::max()));

const char *statusName(QueryStatus status) {
  switch (status) {
  case QueryStatus::Found: return "Found";
  case QueryStatus::NotFound: return "NotFound";
  case QueryStatus::Unknown: return "Unknown";
  }
  return "Unknown";
}

std::string findingSite(const FlowNode &node) {
  const std::string &label = node.label;
  const auto functionEnd = label.find(".temporal:");
  const std::string function = label.substr(0, functionEnd);
  if (!node.sourceLocation.empty())
    return function + ':' + node.sourceLocation;
  return label;
}
} // namespace

int main(int argc, char **argv) {
  llvm::InitLLVM init(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "Lotus SVFG use histories\n");
  cli::QueryOptions options;
  std::string optionError;
  if (!QueryOptionFlags.resolve(options, optionError)) {
    llvm::errs() << "lotus-ir-usetracessa: " << optionError << '\n';
    return 1;
  }
  SearchCompletion completion;
  bool query = Source != std::numeric_limits<unsigned>::max() ||
               Sink != std::numeric_limits<unsigned>::max();
  if ((Source == std::numeric_limits<unsigned>::max()) !=
      (Sink == std::numeric_limits<unsigned>::max())) {
    llvm::errs() << "lotus-ir-usetracessa: supply both --source-node and --sink-node\n";
    return 1;
  }
  if (Format != "text" && Format != "json" && Format != "dot") {
    llvm::errs() << "lotus-ir-usetracessa: --format must be text, json, or dot\n";
    return 1;
  }
  if (DumpMSSA && Format != "text") {
    llvm::errs()
        << "lotus-ir-usetracessa: --dump-mssa requires --format=text\n";
    return 1;
  }
  if (query && Format != "text") {
    llvm::errs() << "lotus-ir-usetracessa: queries require --format=text\n";
    return 1;
  }
  if (!Check.empty() && query) {
    llvm::errs() << "lotus-ir-usetracessa: --check cannot be combined with a node query\n";
    return 1;
  }
  if (!Check.empty() && Format != "text") {
    llvm::errs() << "lotus-ir-usetracessa: --check requires --format=text\n";
    return 1;
  }
  if (!Check.empty() && Check != "double-free" && Check != "use-after-free" &&
      Check != "memory-leak" && Check != "file-leak") {
    llvm::errs() << "lotus-ir-usetracessa: unsupported --check rule\n";
    return 1;
  }

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(Input, diagnostic, context);
  if (!module) {
    diagnostic.print(argv[0], llvm::errs());
    return 1;
  }
  const auto parsed = Clock::now();
  auto millis = [](Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
  };
  if (Timing)
    std::cerr << std::fixed << std::setprecision(3)
              << "timing_ms parse=" << millis(start, parsed) << '\n';
  try {
    ICFG icfg;
    ICFGBuilder icfgBuilder(&icfg);
    icfgBuilder.build(module.get());
    SVFGBuilderConfig config;
    config.usePointerAnalysis = true;
    config.buildMSSA = Check.empty() || DumpMSSA;
    config.resolveIndirectCalls = true;
    SVFGBuilder builder(config);
    std::unique_ptr<SVFG> svfg(builder.build(&icfg));
    const auto builtSVFG = Clock::now();
    if (DumpMSSA) {
      std::string dumpError;
      if (!dumpMemorySSA(*svfg, *module, llvm::outs(), DumpMSSAFunction,
                         &dumpError)) {
        llvm::errs() << "lotus-ir-usetracessa: cannot dump MemorySSA: "
                     << dumpError << "\n";
        return 1;
      }
    }
    if (Timing) std::cerr << "timing_ms svfg=" << millis(parsed, builtSVFG) << '\n';
    if (!DumpSVFG.empty()) {
      std::string dumpError;
      if (!lotus::utils::graphviz::writeSVFGDot(*svfg, *module,
                                                DumpSVFGFunction, DumpSVFG, {},
                                                &dumpError))
        llvm::errs() << "cannot write SVFG DOT: " << dumpError << "\n";
    }
    NativeHistoryMode mode = Check == "double-free" ? NativeHistoryMode::DoubleFree :
                             Check == "use-after-free" ? NativeHistoryMode::UseAfterFree :
                             Check == "memory-leak" ? NativeHistoryMode::MemoryLeak :
                             Check == "file-leak" ? NativeHistoryMode::FileLeak :
                                                          NativeHistoryMode::Full;
    NativeHistoryOptions historyOptions;
    historyOptions.instructionText = Check.empty() || !Quiet || FullLabels;
    if (Timing)
      historyOptions.phaseTiming = [](const char *phase, double elapsed) {
        std::cerr << "timing_ms " << phase << '=' << elapsed << '\n';
      };
    auto result =
        buildUseTraceSSAFromLotusSVFG(*svfg, *module, mode, historyOptions);
    const auto builtHistory = Clock::now();
    if (Timing)
      std::cerr << "timing_ms usetracessa=" << millis(builtSVFG, builtHistory) << '\n';
    std::optional<DefectScan> checkReport;
    std::map<std::string, std::vector<std::size_t>> findingGroups;
    std::map<std::string, std::set<ObjectID>> findingObjects;
    std::optional<QueryResult> nodeQuery;
    if (!Check.empty()) {
      DefectKind kind = Check == "double-free" ? DefectKind::DoubleFree :
                        Check == "use-after-free" ? DefectKind::UseAfterFree :
                        Check == "memory-leak" ? DefectKind::MemoryLeak :
                                                 DefectKind::FileLeak;
      checkReport = DefectDetector(result.graph, options.depth, options.limits,
                                   options.mode)
                        .scan(kind, {!Quiet, !Quiet, false, !Quiet});
      completion.merge(checkReport->completion);
      if (Timing)
        std::cerr << "timing_ms check_search="
                  << checkReport->searchMilliseconds
                  << " check_reporting=" << checkReport->reportingMilliseconds
                  << '\n';
      for (std::size_t index = 0; index < checkReport->findings.size(); ++index) {
        const auto &sink = result.graph.node(checkReport->findings[index].sink);
        std::string site = findingSite(sink);
        findingGroups[site].push_back(index);
        if (!Quiet) {
          const auto &objects = checkReport->findings[index].objects;
          findingObjects[site].insert(objects.begin(), objects.end());
        }
      }
    }
    if (query) {
      auto source = result.native.nodes.find(Source);
      auto sink = result.native.nodes.find(Sink);
      if (source == result.native.nodes.end() || sink == result.native.nodes.end())
        throw std::invalid_argument("source or sink SVFG node was not imported");
      Query request;
      request.sources = {source->second};
      request.sinks = {sink->second};
      options.apply(request);
      nodeQuery = QueryEngine(result.graph).run(request);
      completion.merge(nodeQuery->completion);
    }
    const auto analyzed = Clock::now();
    if (Format == "json") result.graph.printJSON(std::cout);
    else if (Format == "dot") result.graph.printDOT(std::cout);
    else {
      const auto stats = result.graph.statistics();
      std::cout << "svfg_nodes=" << svfg->getNumNodes()
                << " svfg_edges=" << svfg->getStat().numEdges
                << " context_limit=" << options.depthName()
                << " context_mode=" << options.contextName()
                << " history_nodes=" << stats.historyNodes
                << " history_psi=" << stats.historyPsiNodes
                << " history_phi=" << stats.historyPhiNodes
                << " flow_edges=" << stats.flowEdges
                << " guarded_effects=" << stats.guardedEffects
                << " known_object_cardinality=" << stats.knownObjectCardinality
                << " unknown_object_sets=" << stats.unknownObjectSets
                << " issues=" << result.graph.issues().size() << '\n';
      if (!Quiet)
        for (const auto &issue : result.graph.issues())
          std::cout << "issue: " << issue << '\n';
      if (checkReport) {
        const auto &scan = *checkReport;
        std::cout << "check=" << Check << " result=" << statusName(scan.status)
                  << " findings=" << scan.findings.size()
                  << " finding_sites=" << findingGroups.size()
                  << " exhaustive=" << (scan.exhaustive ? "yes" : "no");
        cli::printCompletion(std::cout, scan.completion);
        std::cout << '\n';
        const auto &qs = scan.statistics;
        std::cout << "batch_products=" << qs.productStates
                  << " product_edges=" << qs.productEdges
                  << " edges_examined=" << qs.edgesExamined
                  << " summary_pairs=" << qs.summaryPairs
                  << " mask_intersections=" << qs.maskIntersections
                  << " mask_unions=" << qs.maskUnions
                  << " nonempty_deltas=" << qs.nonemptyDeltas
                  << " candidate_objects=" << qs.candidateObjects
                  << " found_objects=" << qs.foundObjects
                  << " notfound_objects=" << qs.notFoundObjects
                  << " unknown_objects=" << qs.unknownObjects << '\n';
        if (!Quiet && !scan.message.empty())
          std::cout << "message: " << scan.message << '\n';
        if (!Quiet)
          for (const auto &group : findingGroups) {
            std::cout << "finding_site=" << group.first
                      << " candidates=" << group.second.size()
                      << " objects=" << findingObjects.at(group.first).size() << '\n';
            for (auto id : scan.findings[group.second.front()].result.nodes) {
              const auto &node = result.graph.node(id);
              std::cout << "witness_node=" << id << " label=" << node.label;
              auto object = scan.findings[group.second.front()].witnessObject;
              if (object) std::cout << " object=" << *object;
              std::cout << '\n';
            }
          }
      }
      if (nodeQuery) {
        const auto &answer = *nodeQuery;
        std::cout << "query=" << statusName(answer.status)
                  << " witness_edges=" << answer.edges.size()
                  << " product_states=" << answer.productStates
                  << " edges_examined=" << answer.edgesExamined
                  << " summary_pairs=" << answer.summaryPairs;
        cli::printCompletion(std::cout, answer.completion);
        std::cout << '\n';
        if (!Quiet && !answer.message.empty())
          std::cout << "message: " << answer.message << '\n';
        if (!Quiet)
          for (auto node : answer.nodes)
            std::cout << "witness_node=" << node << '\n';
      }
    }
    if (Timing) {
      std::cerr << std::fixed << std::setprecision(3)
                << "timing_ms parse=" << millis(start, parsed)
                << " svfg=" << millis(parsed, builtSVFG)
                << " usetracessa=" << millis(builtSVFG, builtHistory)
                << " check=" << millis(builtHistory, analyzed)
                << " total=" << millis(start, analyzed) << '\n';
    }
    cli::explainIncomplete(std::cerr, completion);
  } catch (const std::exception &error) {
    llvm::errs() << "lotus-ir-usetracessa: " << error.what() << '\n';
    return 1;
  }
  return completion.searchComplete ? 0 : 2;
}
