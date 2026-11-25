/**
 * @file lotus-alias-bootstrap.cpp
 * @brief Driver for bootstrapped flow- and context-sensitive pointer analysis.
 */
#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"

#include <exception>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace {
llvm::cl::OptionCategory
    BootstrapCategory("Lotus bootstrapped pointer analysis");
llvm::cl::opt<std::string> Input(llvm::cl::Positional, llvm::cl::Required,
                                 llvm::cl::desc("<input LLVM IR or bitcode>"),
                                 llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<std::string>
    Entry("entry", llvm::cl::init("main"),
          llvm::cl::desc("Defined program entry function"),
          llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<std::string> SolverKind(
    "solver", llvm::cl::init("guarded"),
    llvm::cl::desc("Cluster solver backend: guarded (PLDI'08 Algs. 2-5) or "
                   "tabulation (reference forward oracle)"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> AllContexts(
    "all-contexts", llvm::cl::init(false),
    llvm::cl::desc("Print the union of reachable contexts in every function"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> PrintAliases(
    "print-aliases", llvm::cl::init(false),
    llvm::cl::desc("Also print Algorithm 3 may-alias sets per pointer"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned>
    Threshold("andersen-threshold", llvm::cl::init(60),
              llvm::cl::desc("Andersen refinement threshold"),
              llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> AdaptiveThreshold(
    "adaptive-threshold", llvm::cl::init(true),
    llvm::cl::desc("Adapt the Andersen threshold from observed refinement"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> AdaptiveGlobalEscalation(
    "adaptive-global-escalation", llvm::cl::init(false),
    llvm::cl::desc("Escalate threshold globally upon a rejected partition "
                   "(false = per-partition rejection only)"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned> HybridTabulationMaxCluster(
    "hybrid-tabulation-max-cluster", llvm::cl::init(0),
    llvm::cl::desc("Hybrid mode: use tabulation solver for clusters of size <= N "
                   "(0 disables)"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> OneLevelFlow(
    "one-level-flow", llvm::cl::init(false),
    llvm::cl::desc("Enable Das (2000) one-level-flow cascade stage between "
                   "Steensgaard and Andersen"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> PathSensitive(
    "path-sensitive", llvm::cl::init(false),
    llvm::cl::desc("Enable ROBDD-based path sensitivity (Kahlon §3 p. 258)"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> PathSensitiveInterproc(
    "path-sensitive-interproc", llvm::cl::init(false),
    llvm::cl::desc("Propagate ROBDD branching constraints across functions"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> EagerDovetail(
    "eager-dovetail", llvm::cl::init(false),
    llvm::cl::desc("Eagerly precompute function summaries per Steensgaard "
                   "depth (literal Algorithm 2 lines 2 & 8)"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned> MaxAndersenPartition(
    "max-andersen-partition", llvm::cl::init(4096),
    llvm::cl::desc("Skip adaptive refinement above this size (zero disables)"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned> MaxAndersenWork(
    "max-andersen-work", llvm::cl::init(4 * 1024 * 1024),
    llvm::cl::desc("Adaptive partition x hierarchy work budget"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> ParallelClusters(
    "parallel-clusters", llvm::cl::init(true),
    llvm::cl::desc("Evaluate independent query clusters in parallel"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool>
    PrecomputeClusters("precompute-clusters", llvm::cl::init(false),
                       llvm::cl::desc("Eagerly construct all cluster solvers"),
                       llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned> Threads(
    "threads", llvm::cl::init(0),
    llvm::cl::desc("Cluster worker count (zero uses hardware concurrency)"),
    llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned>
    ContextLimit("max-contexts", llvm::cl::init(4096),
                 llvm::cl::desc("Maximum summary contexts per cluster"),
                 llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<unsigned>
    StepLimit("max-steps", llvm::cl::init(1000000),
              llvm::cl::desc("Maximum solver steps per cluster"),
              llvm::cl::cat(BootstrapCategory));
llvm::cl::opt<bool> DetailedStats(
    "detailed-stats", llvm::cl::init(false),
    llvm::cl::desc("Print partition and cluster size distributions"),
    llvm::cl::cat(BootstrapCategory));
} // namespace

int main(int argc, char **argv) {
  llvm::InitLLVM init(argc, argv);
  llvm::cl::HideUnrelatedOptions(BootstrapCategory);
  llvm::cl::ParseCommandLineOptions(
      argc, argv, "Bootstrapped flow/context-sensitive points-to analysis\n");
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(Input, diagnostic, context);
  if (!module) {
    diagnostic.print(argv[0], llvm::errs());
    return 1;
  }
  const llvm::Function *entry = module->getFunction(Entry);
  if (!entry || entry->isDeclaration()) {
    llvm::errs() << "No defined entry function: " << Entry << '\n';
    return 1;
  }
  try {
    lotus::bootstrap::Options options;
    if (SolverKind == "guarded") {
      options.solver_mode = lotus::bootstrap::SolverMode::GuardedSummary;
    } else if (SolverKind == "tabulation") {
      options.solver_mode = lotus::bootstrap::SolverMode::InputStateTabulation;
    } else {
      llvm::errs() << "Unknown --solver value: " << SolverKind
                   << " (expected guarded or tabulation)\n";
      return 1;
    }
    options.andersen_threshold = Threshold;
    options.adaptive_andersen_threshold = AdaptiveThreshold;
    options.adaptive_global_threshold_escalation = AdaptiveGlobalEscalation;
    options.hybrid_tabulation_max_cluster_size = HybridTabulationMaxCluster;
    options.enable_one_level_flow = OneLevelFlow;
    options.path_sensitive = PathSensitive || PathSensitiveInterproc;
    options.path_sensitive_interprocedural = PathSensitiveInterproc;
    options.eager_dovetail_summaries = EagerDovetail;
    options.max_andersen_partition_size = MaxAndersenPartition;
    options.max_andersen_work = MaxAndersenWork;
    options.parallel_clusters = ParallelClusters;
    options.parallelism = Threads;
    options.max_contexts = ContextLimit;
    options.max_steps = StepLimit;
    lotus::bootstrap::BootstrapAA analysis(*module, entry, options);
    if (PrecomputeClusters || AllContexts)
      analysis.precomputeAll();
    bool limited = false;
    for (const llvm::Function &function : *module) {
      if (!AllContexts && &function != entry)
        continue;
      for (const llvm::BasicBlock &block : function) {
        for (const llvm::Instruction &instruction : block) {
          if (!instruction.getType()->isPointerTy())
            continue;
          auto result =
              AllContexts
                  ? analysis.pointsToAllContexts(instruction, instruction,
                                                 lotus::bootstrap::Point::After)
                  : analysis.pointsTo(instruction, instruction, {},
                                      lotus::bootstrap::Point::After);
          llvm::outs() << function.getName() << ": ";
          instruction.printAsOperand(llvm::outs(), false);
          llvm::outs() << " -> ";
          if (!result.reachable()) {
            llvm::outs() << "<unreachable>\n";
            continue;
          }
          llvm::outs() << '{';
          if (result.points_to.isTop()) {
            llvm::outs() << "TOP";
          } else {
            bool first = true;
            for (lotus::bootstrap::Id object : result.points_to.objects()) {
              if (!first)
                llvm::outs() << ", ";
              first = false;
              llvm::outs() << analysis.objectInfo(object).name;
            }
          }
          llvm::outs() << '}';
          if (result.status == lotus::bootstrap::QueryStatus::ResourceLimit) {
            limited = true;
            llvm::outs() << " [resource-limit fallback]";
          }
          if (PrintAliases) {
            auto aliasRes =
                AllContexts
                    ? analysis.aliasesAllContexts(
                          instruction, instruction,
                          lotus::bootstrap::Point::After)
                    : analysis.aliases(instruction, instruction, {},
                                       lotus::bootstrap::Point::After);
            llvm::outs() << " aliases={";
            bool afirst = true;
            for (const llvm::Value *av : aliasRes.aliases) {
              if (!afirst)
                llvm::outs() << ", ";
              afirst = false;
              av->printAsOperand(llvm::outs(), false);
            }
            if (aliasRes.status == lotus::bootstrap::QueryStatus::ResourceLimit)
              limited = true;
            if (aliasRes.may_alias_all)
              llvm::outs() << (afirst ? "*" : ", *");
            llvm::outs() << '}';
          }
          llvm::outs() << '\n';
        }
      }
    }
    const auto &stats = analysis.statistics();
    llvm::errs() << "partitions=" << stats.steensgaard_partitions
                 << " largest-partition=" << stats.steensgaard_largest_partition
                 << " hierarchy-nodes=" << stats.hierarchy_nodes
                 << " hierarchy-edges=" << stats.hierarchy_edges
                 << " hierarchy-depth=" << stats.hierarchy_max_depth
                 << " cyclic-components=" << stats.hierarchy_cyclic_components
                 << " one-level-flow-runs=" << stats.one_level_flow_runs
                 << " andersen-runs=" << stats.andersen_runs
                 << " adaptive-skips=" << stats.adaptive_refinement_skips
                 << " adaptive-rejections="
                 << stats.adaptive_refinement_rejections
                 << " adaptive-cost-skips=" << stats.adaptive_cost_skips
                 << " effective-threshold="
                 << stats.effective_andersen_threshold
                 << " clusters=" << stats.clusters
                 << " largest-cluster=" << stats.largest_cluster
                 << " cover-memberships=" << stats.cover_memberships
                 << " overlapping-values=" << stats.overlapping_values
                 << " max-cover-memberships=" << stats.maximum_cover_memberships
                 << " evaluated-clusters=" << stats.evaluated_clusters
                 << " parallel-tasks=" << stats.parallel_cluster_tasks
                 << " call-graph-sccs=" << stats.call_graph_sccs
                 << " recursive-sccs=" << stats.recursive_call_graph_sccs
                 << " scc-reschedules=" << stats.scc_reschedules
                 << " dovetail-levels=" << stats.dovetail_levels
                 << " fsci-queries=" << stats.fsci_queries
                 << " summary-tuples=" << stats.summary_tuples
                 << " guard-atoms=" << stats.guard_atoms_created
                 << " guards-pruned=" << stats.guards_pruned
                 << " guards-widened=" << stats.guards_widened
                 << " skipped-functions=" << stats.skipped_functions
                 << " path-prunes=" << stats.path_infeasible_prunes
                 << " bdd-nodes=" << stats.bdd_nodes
                 << " contexts=" << stats.contexts
                 << " context-cache-hits=" << stats.context_cache_hits
                 << " context-cache-misses=" << stats.context_cache_misses
                 << " max-contexts-per-cluster="
                 << stats.maximum_contexts_per_cluster
                 << " summary-updates=" << stats.summary_updates
                 << " preprocessing-ms=" << stats.preprocessing_milliseconds
                 << " steps=" << stats.steps
                 << " fallback-queries=" << stats.fallback_queries << '\n';
    auto printSizes = [](llvm::StringRef label,
                         const std::vector<std::size_t> &sizes) {
      llvm::errs() << label << "={";
      for (std::size_t index = 0; index < sizes.size(); ++index) {
        if (index)
          llvm::errs() << ',';
        llvm::errs() << sizes[index];
      }
      llvm::errs() << "}\n";
    };
    if (DetailedStats) {
      printSizes("steensgaard-partition-sizes",
                 stats.steensgaard_partition_sizes);
      printSizes("andersen-cluster-sizes", stats.cluster_sizes);
      llvm::errs() << "cluster-solve-ms={";
      for (std::size_t index = 0;
           index < stats.cluster_solve_milliseconds.size(); ++index) {
        if (index)
          llvm::errs() << ',';
        llvm::errs() << stats.cluster_solve_milliseconds[index];
      }
      llvm::errs() << "}\n";
    }
    return limited ? 2 : 0;
  } catch (const std::exception &error) {
    llvm::errs() << "BootstrapAA: " << error.what() << '\n';
    return 1;
  }
}
