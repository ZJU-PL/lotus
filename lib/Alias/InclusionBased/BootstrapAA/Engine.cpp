#include "Alias/InclusionBased/BootstrapAA/Engine.h"
#include "Alias/InclusionBased/BootstrapAA/IR/ProgramIndex.h"
#include "Alias/InclusionBased/BootstrapAA/Preprocessing/Partitioning.h"
#include "Alias/InclusionBased/BootstrapAA/Preprocessing/RelevantSlice.h"
#include "Alias/InclusionBased/BootstrapAA/Solver/ClusterSolver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <stdexcept>
#include <thread>

namespace lotus {
namespace bootstrap {

using namespace detail;

struct Analysis::Impl {
  Program program;
  Options options;
  Statistics stats;
  ProgramIndex index;
  Sets upper;
  Sets object_upper;
  SteensgaardHierarchy hierarchy;
  CallGraphInfo call_graph;
  ReachabilityInfo reachability;
  SliceIndex slice_index;
  std::vector<std::vector<Id>> cover;
  std::vector<std::unique_ptr<ClusterSolver>> solvers;
  std::vector<Statistics> merged_solver_stats;
  std::map<Id, std::unique_ptr<ClusterSolver>> object_solvers;
  std::map<Id, Statistics> merged_object_solver_stats;
  std::map<Id, std::pair<Id, bool>> sites;
  // Non-constant values per function whose refined upper bound is empty:
  // reachable queries on such uninitialized values widen to Top, so non-null
  // alias queries in the same function must include them symmetrically with
  // `top_component` and `mayAlias`.
  std::vector<std::vector<Id>> empty_upper_values_by_fn;
  // Non-constant values per function whose refined upper bound is Top (kept in
  // their own cluster rather than duplicated into every concrete object cluster
  // of buildInverseCover, and included in finalizeAliasResult for symmetry).
  std::vector<std::vector<Id>> top_upper_values_by_fn;
  // Non-null-capable local SSA values and referenced constants per function
  // (for full `aliases()` symmetry when `may_alias_all == true`).
  std::vector<std::vector<Id>> non_null_values_by_fn;
  std::vector<std::vector<Id>> non_null_constants_by_fn;
  // Pointer-holding memory objects that can hold a non-null target in
  // Steensgaard, plus reverse index by target Steensgaard component.
  std::vector<Id> non_null_ptr_mem_objects;
  std::vector<std::vector<Id>> ptr_mem_objs_by_target_comp;

  static Program prepareProgram(const Program &p, HavocPolicy policy) {
    p.validate();
    Program out = p;
    if (policy == HavocPolicy::Noop) {
      for (Function &fn : out.functions) {
        if (fn.external)
          fn.writes_memory = false;
        for (Block &bb : fn.blocks) {
          for (Instruction &inst : bb.instructions) {
            if (inst.opcode == Opcode::Havoc && !inst.zero_fill) {
              inst.opcode = Opcode::Nop;
              inst.result = INVALID;
              inst.operands.clear();
              inst.write_bytes = 0;
            } else if (inst.opcode == Opcode::Call) {
              inst.writes_memory = false;
              inst.opaque_alternative = false;
            }
          }
        }
      }
    }
    return out;
  }

  Impl(const Program &p, Options o)
      : program(prepareProgram(p, o.havoc_policy)), options(o), index(program) {
    const auto preprocessingStart = std::chrono::steady_clock::now();
    forInstructions(program, [&](Id f, const Instruction &i) {
      sites.emplace(i.id, std::make_pair(f, i.opcode == Opcode::Call));
    });

    // Stage 1: Steensgaard partitioning & retained points-to hierarchy (§2.1).
    CoarseResult coarse = steensgaard(program, options.havoc_policy);
    upper = coarse.upper;
    object_upper = std::move(coarse.object_upper);
    hierarchy = std::move(coarse.hierarchy);
    index.populateCallers(program, upper, hierarchy);
    reachability = computeReachability(program, index, upper);

    stats.steensgaard_partitions = coarse.partitions.size();
    for (const auto &partition : coarse.partitions) {
      stats.steensgaard_partition_sizes.push_back(partition.size());
      stats.steensgaard_largest_partition =
          std::max(stats.steensgaard_largest_partition, partition.size());
    }
    stats.hierarchy_nodes = hierarchy.successors.size();
    for (Id node = 0; node < hierarchy.successors.size(); ++node) {
      stats.hierarchy_edges += hierarchy.successors[node].size();
      stats.hierarchy_max_depth =
          std::max(stats.hierarchy_max_depth, hierarchy.depth[node]);
      stats.hierarchy_cyclic_components += hierarchy.cyclic[node] ? 1 : 0;
    }

    auto buildInverseCover = [&](const std::vector<Id> &part,
                                 const Sets &refined) {
      std::map<Id, std::vector<Id>> inverse;
      std::vector<std::vector<Id>> out;
      std::vector<Id> top_vals;
      for (Id v : part) {
        if (refined[v].isTop()) {
          top_vals.push_back(v);
        } else {
          // Exclude NULL_OBJECT from the inverse points-to cover so nullable
          // pointers do not collapse back into one giant cluster (matching
          // Steensgaard's no-NULL-bridge partitioning and non-null alias
          // semantics).
          bool added_non_null = false;
          for (Id object : refined[v].objects()) {
            if (object == NULL_OBJECT)
              continue;
            inverse[object].push_back(v);
            added_non_null = true;
          }
          if (!added_non_null)
            out.push_back({v});
        }
      }
      if (!top_vals.empty()) {
        std::sort(top_vals.begin(), top_vals.end());
        top_vals.erase(std::unique(top_vals.begin(), top_vals.end()),
                       top_vals.end());
        out.push_back(std::move(top_vals));
      }
      for (auto &cluster : inverse)
        if (!cluster.second.empty())
          out.push_back(std::move(cluster.second));
      return out;
    };

    if (!options.enable_clustering) {
      cover.emplace_back(program.values.size());
      std::iota(cover.back().begin(), cover.back().end(), 0);
      stats.effective_andersen_threshold = options.andersen_threshold;
    } else {
      std::vector<std::vector<Id>> partitions = coarse.partitions;
      SliceIndex coarse_slice_index;
      bool coarse_slice_index_ready = false;
      auto getCoarseSliceIndex = [&]() -> const SliceIndex * {
        if (!coarse_slice_index_ready) {
          coarse_slice_index = buildSliceIndex(program, index, coarse.upper,
                                               hierarchy, reachability);
          coarse_slice_index_ready = true;
        }
        return &coarse_slice_index;
      };

      // Optional intermediate cascade stage: Das (2000) One-Level Flow
      // between Steensgaard and Andersen (paper §4, p. 258).
      if (options.enable_one_level_flow) {
        std::vector<std::vector<Id>> flow_refined;
        for (const auto &partition : partitions) {
          if (partition.size() <= options.andersen_threshold) {
            flow_refined.push_back(partition);
            continue;
          }
          ++stats.one_level_flow_runs;
          Slice olf_slice =
              relevant(program, index, coarse.upper, hierarchy, reachability,
                       partition, true, {}, getCoarseSliceIndex());
          Sets olf = oneLevelFlow(program, index, olf_slice, coarse.upper,
                                  options.havoc_policy);
          auto sub = buildInverseCover(partition, olf);
          std::size_t sub_largest = 0;
          for (const auto &c : sub)
            sub_largest = std::max(sub_largest, c.size());
          for (Id v : partition)
            upper[v] = olf[v];
          if (sub_largest < partition.size()) {
            flow_refined.insert(flow_refined.end(), sub.begin(), sub.end());
          } else {
            flow_refined.push_back(partition);
          }
        }
        std::sort(flow_refined.begin(), flow_refined.end());
        flow_refined.erase(
            std::unique(flow_refined.begin(), flow_refined.end()),
            flow_refined.end());
        partitions = std::move(flow_refined);
      }

      if (options.adaptive_andersen_threshold)
        std::stable_sort(partitions.begin(), partitions.end(),
                         [](const auto &left, const auto &right) {
                           return left.size() > right.size();
                         });

      std::size_t threshold = options.andersen_threshold;
      const long double rejectRatio =
          static_cast<long double>(options.andersen_rejection_retention_ratio);
      for (const auto &partition : partitions) {
        const bool workLimitExceeded =
            options.max_andersen_work != 0 && !hierarchy.successors.empty() &&
            partition.size() >
                options.max_andersen_work / hierarchy.successors.size();
        if (options.adaptive_andersen_threshold &&
            ((options.max_andersen_partition_size != 0 &&
              partition.size() > options.max_andersen_partition_size) ||
             workLimitExceeded)) {
          ++stats.adaptive_cost_skips;
          cover.push_back(partition);
          if (options.adaptive_global_threshold_escalation)
            threshold = std::max(threshold, partition.size());
          continue;
        }
        if (partition.size() <= threshold) {
          if (partition.size() > options.andersen_threshold)
            ++stats.adaptive_refinement_skips;
          cover.push_back(partition);
          continue;
        }
        ++stats.andersen_runs;
        Slice slice =
            relevant(program, index, coarse.upper, hierarchy, reachability,
                     partition, true, {}, getCoarseSliceIndex());
        Sets refined = inclusion(program, index, slice, options.havoc_policy);
        auto refinedCover = buildInverseCover(partition, refined);
        std::size_t refinedLargest = 0;
        for (const auto &cluster : refinedCover)
          refinedLargest = std::max(refinedLargest, cluster.size());
        if (!options.adaptive_andersen_threshold) {
          for (Id value : partition)
            upper[value] = refined[value];
          // Paper §4 p. 259: avoid replacing a partition with overlapping
          // clusters when Andersen produces no reduction in max cluster size.
          if (refinedLargest < partition.size()) {
            cover.insert(cover.end(), refinedCover.begin(), refinedCover.end());
          } else {
            ++stats.adaptive_refinement_rejections;
            cover.push_back(partition);
          }
          continue;
        }
        const long double retainedFraction =
            partition.empty() ? 0.0L
                              : static_cast<long double>(refinedLargest) /
                                    static_cast<long double>(partition.size());
        if (retainedFraction >= rejectRatio) {
          ++stats.adaptive_refinement_rejections;
          cover.push_back(partition);
          if (options.adaptive_global_threshold_escalation)
            threshold = std::max(threshold, partition.size());
        } else {
          for (Id value : partition)
            upper[value] = refined[value];
          cover.insert(cover.end(), refinedCover.begin(), refinedCover.end());
          if (options.adaptive_global_threshold_escalation &&
              retainedFraction <= 0.5L)
            threshold = std::max<std::size_t>(1, threshold * 3 / 4);
        }
      }
      stats.effective_andersen_threshold = threshold;
    }

    call_graph = buildCallGraph(program, upper);
    // Demote singleton flag on any stack allocation belonging to a recursive
    // SCC of the Steensgaard/Andersen-resolved call graph (including functions
    // reached via Top indirect calls), preserving strong updates on stack
    // allocas in provably non-recursive functions when indirect calls resolve.
    auto demoteStackBase = [&](Id obj) {
      if (obj >= program.objects.size() ||
          program.objects[obj].kind != ObjectKind::Stack)
        return;
      const Id base = program.baseOf(obj);
      program.objects[base].singleton = false;
      if (base < program.fields_of_base.size()) {
        for (auto [off, fid] : program.fields_of_base[base]) {
          (void)off;
          if (fid < program.objects.size())
            program.objects[fid].singleton = false;
        }
      }
    };
    for (Id o = 2; o < program.objects.size(); ++o) {
      const Object &obj = program.objects[o];
      if (obj.kind == ObjectKind::Stack && obj.function != INVALID &&
          obj.function < call_graph.function_scc.size() &&
          call_graph.scc_recursive[call_graph.function_scc[obj.function]])
        demoteStackBase(o);
    }
    for (Id alloc_site : index.allocate_sites) {
      const InstLocation &loc = index.by_site[alloc_site];
      if (loc.function < call_graph.function_scc.size() &&
          call_graph.scc_recursive[call_graph.function_scc[loc.function]])
        demoteStackBase(loc.inst->object);
    }

    index.populateCallers(program, upper, hierarchy);
    reachability = computeReachability(program, index, upper);
    if (options.enable_slicing)
      slice_index =
          buildSliceIndex(program, index, upper, hierarchy, reachability);
    auto hasNonNullPts = [](const PointsToSet &pts) -> bool {
      if (pts.isTop())
        return true;
      for (Id o : pts.objects())
        if (o != NULL_OBJECT)
          return true;
      return false;
    };
    empty_upper_values_by_fn.assign(program.functions.size(), {});
    top_upper_values_by_fn.assign(program.functions.size(), {});
    non_null_values_by_fn.assign(program.functions.size(), {});
    non_null_constants_by_fn.assign(program.functions.size(), {});
    for (Id v = 0; v < program.values.size(); ++v) {
      const Value &vinfo = program.values[v];
      if (vinfo.constant || vinfo.function == INVALID ||
          vinfo.function >= program.functions.size())
        continue;
      if (upper[v].empty()) {
        empty_upper_values_by_fn[vinfo.function].push_back(v);
        non_null_values_by_fn[vinfo.function].push_back(v);
      } else if (upper[v].isTop()) {
        top_upper_values_by_fn[vinfo.function].push_back(v);
        non_null_values_by_fn[vinfo.function].push_back(v);
      } else if (hasNonNullPts(upper[v])) {
        non_null_values_by_fn[vinfo.function].push_back(v);
      }
    }
    for (Id f = 0; f < program.functions.size(); ++f) {
      std::set<Id> fn_consts;
      auto recordConst = [&](Id v) {
        if (v != INVALID && v < program.values.size() &&
            program.values[v].constant &&
            (program.values[v].initial.empty() ||
             hasNonNullPts(program.values[v].initial)))
          fn_consts.insert(v);
      };
      for (const Block &block : program.functions[f].blocks) {
        for (const Instruction &inst : block.instructions) {
          recordConst(inst.indirect_target);
          for (Id op : inst.operands)
            recordConst(op);
        }
        for (const Edge &edge : block.successors)
          for (const auto &phi : edge.phi)
            recordConst(phi.second);
      }
      non_null_constants_by_fn[f].assign(fn_consts.begin(), fn_consts.end());
    }

    const std::size_t ncomp = hierarchy.successors.size();
    std::vector<bool> comp_has_non_null_obj(ncomp, false);
    for (Id o = 0; o < program.objects.size(); ++o) {
      if (o == NULL_OBJECT)
        continue;
      if (o < hierarchy.object_components.size() &&
          hierarchy.object_components[o] < ncomp)
        comp_has_non_null_obj[hierarchy.object_components[o]] = true;
    }
    ptr_mem_objs_by_target_comp.assign(ncomp, {});
    for (Id o = 2; o < program.objects.size(); ++o) {
      const Object &oinfo = program.objects[o];
      if (!isMemory(oinfo) || (oinfo.bytes == 0 && oinfo.size_bytes != 0))
        continue;
      const Id ocomp = hierarchy.object_components[o];
      bool can_hold_non_null = false;
      auto registerTargetComp = [&](Id tcomp) {
        if (tcomp < ncomp && comp_has_non_null_obj[tcomp]) {
          can_hold_non_null = true;
          ptr_mem_objs_by_target_comp[tcomp].push_back(o);
        }
      };
      if (hierarchy.cyclic[ocomp])
        registerTargetComp(ocomp);
      for (Id succ : hierarchy.successors[ocomp])
        registerTargetComp(succ);
      if (can_hold_non_null)
        non_null_ptr_mem_objects.push_back(o);
    }
    stats.call_graph_sccs = call_graph.scc_count;
    stats.recursive_call_graph_sccs = call_graph.recursive_scc_count;

    // Canonicalize Disjunctive Alias Cover without truncating overlap.
    for (auto &cluster : cover) {
      std::sort(cluster.begin(), cluster.end());
      cluster.erase(std::unique(cluster.begin(), cluster.end()), cluster.end());
    }
    std::sort(cover.begin(), cover.end());
    cover.erase(std::unique(cover.begin(), cover.end()), cover.end());

    stats.clusters = cover.size();
    std::vector<std::size_t> memberships(program.values.size(), 0);
    for (const auto &cluster : cover) {
      stats.cluster_sizes.push_back(cluster.size());
      stats.largest_cluster = std::max(stats.largest_cluster, cluster.size());
      stats.cover_memberships += cluster.size();
      for (Id value : cluster)
        ++memberships[value];
    }
    for (std::size_t count : memberships) {
      stats.overlapping_values += count > 1 ? 1 : 0;
      stats.maximum_cover_memberships =
          std::max(stats.maximum_cover_memberships, count);
    }

    solvers.resize(cover.size());
    merged_solver_stats.resize(cover.size());
    stats.cluster_solve_milliseconds.assign(cover.size(), 0.0);
    stats.preprocessing_milliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - preprocessingStart)
            .count();
  }

  struct BuiltSolver {
    std::size_t cluster = 0;
    std::unique_ptr<ClusterSolver> solver;
    double milliseconds = 0.0;
  };

  SolverMode chooseSolverMode(std::size_t cluster, std::size_t cluster_size,
                              std::size_t slice_statements) const {
    if (options.cluster_solver_selector)
      return options.cluster_solver_selector(cluster, cluster_size,
                                             slice_statements);
    if (options.hybrid_tabulation_max_cluster_size > 0 &&
        cluster_size <= options.hybrid_tabulation_max_cluster_size &&
        (options.hybrid_tabulation_max_slice_statements == 0 ||
         slice_statements <= options.hybrid_tabulation_max_slice_statements))
      return SolverMode::InputStateTabulation;
    return options.solver_mode;
  }

  BuiltSolver buildSolver(std::size_t cluster) const {
    const auto started = std::chrono::steady_clock::now();
    BuiltSolver built;
    built.cluster = cluster;
    Slice slice = relevant(program, index, upper, hierarchy, reachability,
                           cover[cluster], options.enable_slicing, {},
                           options.enable_slicing ? &slice_index : nullptr);
    const SolverMode mode =
        chooseSolverMode(cluster, cover[cluster].size(), slice.inst_list.size());
    if (mode == SolverMode::InputStateTabulation) {
      built.solver = createTabulationSolver(program, index, upper, call_graph,
                                            cover[cluster], std::move(slice),
                                            options);
    } else {
      built.solver = createGuardedSummarySolver(
          program, index, upper, hierarchy, call_graph, reachability,
          cover[cluster], std::move(slice), options);
    }
    built.milliseconds = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    return built;
  }

  void mergeStatsDelta(Statistics &prev, const Statistics &cur) {
    auto addDelta = [](std::size_t &agg, std::size_t &old_val,
                       std::size_t new_val) {
      if (new_val > old_val) {
        agg += (new_val - old_val);
        old_val = new_val;
      }
    };
    addDelta(stats.slice_values, prev.slice_values, cur.slice_values);
    addDelta(stats.slice_objects, prev.slice_objects, cur.slice_objects);
    addDelta(stats.slice_statements, prev.slice_statements,
             cur.slice_statements);
    addDelta(stats.contexts, prev.contexts, cur.contexts);
    addDelta(stats.context_cache_hits, prev.context_cache_hits,
             cur.context_cache_hits);
    addDelta(stats.context_cache_misses, prev.context_cache_misses,
             cur.context_cache_misses);
    stats.maximum_contexts_per_cluster = std::max(
        stats.maximum_contexts_per_cluster, cur.maximum_contexts_per_cluster);
    addDelta(stats.summary_updates, prev.summary_updates, cur.summary_updates);
    addDelta(stats.steps, prev.steps, cur.steps);
    stats.call_graph_sccs =
        std::max(stats.call_graph_sccs, cur.call_graph_sccs);
    stats.recursive_call_graph_sccs = std::max(stats.recursive_call_graph_sccs,
                                               cur.recursive_call_graph_sccs);
    addDelta(stats.scc_reschedules, prev.scc_reschedules, cur.scc_reschedules);
    addDelta(stats.dovetail_levels, prev.dovetail_levels, cur.dovetail_levels);
    addDelta(stats.fsci_queries, prev.fsci_queries, cur.fsci_queries);
    addDelta(stats.fsci_cache_hits, prev.fsci_cache_hits, cur.fsci_cache_hits);
    addDelta(stats.summary_tuples, prev.summary_tuples, cur.summary_tuples);
    addDelta(stats.guard_atoms_created, prev.guard_atoms_created,
             cur.guard_atoms_created);
    addDelta(stats.guards_pruned, prev.guards_pruned, cur.guards_pruned);
    addDelta(stats.guards_widened, prev.guards_widened, cur.guards_widened);
    addDelta(stats.skipped_functions, prev.skipped_functions,
             cur.skipped_functions);
    addDelta(stats.path_infeasible_prunes, prev.path_infeasible_prunes,
             cur.path_infeasible_prunes);
    stats.bdd_nodes = std::max(stats.bdd_nodes, cur.bdd_nodes);
  }

  void syncSolverStatistics(std::size_t cluster) {
    if (!solvers[cluster])
      return;
    mergeStatsDelta(merged_solver_stats[cluster],
                    solvers[cluster]->statistics());
  }

  void ensureSolvers(const std::vector<std::size_t> &selected) {
    std::vector<std::size_t> missing;
    for (std::size_t cluster : selected)
      if (!solvers[cluster])
        missing.push_back(cluster);
    if (missing.empty())
      return;

    std::vector<BuiltSolver> built(missing.size());
    std::size_t workers = options.parallelism;
    if (workers == 0)
      workers = std::thread::hardware_concurrency();
    workers = std::max<std::size_t>(1, std::min(workers, missing.size()));
    if (!options.parallel_clusters)
      workers = 1;
    if (workers == 1) {
      for (std::size_t idx = 0; idx < missing.size(); ++idx)
        built[idx] = buildSolver(missing[idx]);
    } else {
      // Load-balanced parallel scheduling (§4): dispatch largest clusters
      // first so heavy clusters do not create tail stalls across workers.
      std::sort(missing.begin(), missing.end(),
                [&](std::size_t a, std::size_t b) {
                  if (cover[a].size() != cover[b].size())
                    return cover[a].size() > cover[b].size();
                  return a < b;
                });
      stats.parallel_cluster_tasks += missing.size();
      std::atomic<std::size_t> next{0};
      std::exception_ptr failure;
      std::mutex failureMutex;
      std::vector<std::thread> threads;
      threads.reserve(workers);
      for (std::size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&] {
          while (true) {
            const std::size_t idx = next.fetch_add(1);
            if (idx >= missing.size())
              return;
            try {
              built[idx] = buildSolver(missing[idx]);
            } catch (...) {
              std::lock_guard<std::mutex> lock(failureMutex);
              if (!failure)
                failure = std::current_exception();
              return;
            }
          }
        });
      }
      for (std::thread &thread : threads)
        thread.join();
      if (failure)
        std::rethrow_exception(failure);
    }

    // Publish results and aggregate counters deterministically in cluster order.
    std::sort(built.begin(), built.end(),
              [](const BuiltSolver &left, const BuiltSolver &right) {
                return left.cluster < right.cluster;
              });
    for (BuiltSolver &part : built) {
      stats.cluster_solve_milliseconds[part.cluster] = part.milliseconds;
      solvers[part.cluster] = std::move(part.solver);
      syncSolverStatistics(part.cluster);
      ++stats.evaluated_clusters;
    }
  }

  void precomputeAll() {
    std::vector<std::size_t> selected(cover.size());
    std::iota(selected.begin(), selected.end(), 0);
    ensureSolvers(selected);
  }

  void precomputeClusters(const std::vector<std::size_t> &cluster_indices) {
    std::vector<std::size_t> selected;
    selected.reserve(cluster_indices.size());
    for (std::size_t c : cluster_indices) {
      if (c >= cover.size())
        throw std::invalid_argument("unknown cluster index");
      selected.push_back(c);
    }
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()),
                   selected.end());
    ensureSolvers(selected);
  }

  void precomputeForValues(const std::vector<Id> &values) {
    std::vector<std::size_t> selected;
    for (Id v : values) {
      if (v >= program.values.size())
        throw std::invalid_argument("unknown query value");
      auto cs = selectClusters(v);
      selected.insert(selected.end(), cs.begin(), cs.end());
    }
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()),
                   selected.end());
    ensureSolvers(selected);
  }

  void validateSiteAndContext(Id site, const Context *context_path) const {
    if (!sites.count(site))
      throw std::invalid_argument("unknown query instruction");
    if (context_path) {
      for (Id callsite : *context_path) {
        auto it = sites.find(callsite);
        if (it == sites.end() || !it->second.second)
          throw std::invalid_argument("context contains a non-call site");
      }
    }
  }

  void validateQueryArgs(Id value, Id site, const Context *context_path) const {
    if (value >= program.values.size())
      throw std::invalid_argument("unknown query value or instruction");
    validateSiteAndContext(site, context_path);
  }

  ClusterSolver &ensureObjectSolver(Id object) {
    auto it = object_solvers.find(object);
    if (it != object_solvers.end())
      return *it->second;
    const Id ocomp = hierarchy.object_components[object];
    std::vector<Id> cluster_vals;
    auto appendCompVals = [&](Id comp) {
      if (comp < hierarchy.values.size())
        cluster_vals.insert(cluster_vals.end(), hierarchy.values[comp].begin(),
                            hierarchy.values[comp].end());
    };
    if (hierarchy.cyclic[ocomp])
      appendCompVals(ocomp);
    for (Id succ : hierarchy.successors[ocomp])
      appendCompVals(succ);
    std::sort(cluster_vals.begin(), cluster_vals.end());
    cluster_vals.erase(std::unique(cluster_vals.begin(), cluster_vals.end()),
                       cluster_vals.end());
    Slice slice = relevant(program, index, upper, hierarchy, reachability,
                           cluster_vals, options.enable_slicing, {object},
                           options.enable_slicing ? &slice_index : nullptr);
    std::unique_ptr<ClusterSolver> solver;
    if (options.solver_mode == SolverMode::InputStateTabulation) {
      solver = createTabulationSolver(program, index, upper, call_graph,
                                      cluster_vals, std::move(slice), options);
    } else {
      solver = createGuardedSummarySolver(
          program, index, upper, hierarchy, call_graph, reachability,
          cluster_vals, std::move(slice), options);
    }
    auto [inserted, _] = object_solvers.emplace(object, std::move(solver));
    merged_object_solver_stats[object] = {};
    mergeStatsDelta(merged_object_solver_stats[object],
                    inserted->second->statistics());
    return *inserted->second;
  }

  PointsToSet fallbackPointsTo(Id value) const {
    if (value < upper.size()) {
      const PointsToSet &u = upper[value];
      if (!u.empty() || program.values[value].constant)
        return u;
    }
    return PointsToSet::top();
  }

  PointsToSet fallbackObjectPointsTo(Id object) const {
    if (object < object_upper.size() && !object_upper[object].empty())
      return object_upper[object];
    return PointsToSet::top();
  }

  AliasResult buildFallbackAliases(const PointsToSet &base_pts, Id self_val,
                                   Id self_obj, Id site, Point point) const {
    AliasResult fb;
    fb.status = QueryStatus::ResourceLimit;
    if (self_val != INVALID)
      fb.aliases.push_back(self_val);
    if (self_obj != INVALID)
      fb.object_aliases.push_back(self_obj);
    if (base_pts.isTop()) {
      fb.may_alias_all = true;
      finalizeAliasResult(fb, PointsToSet::top(), site, point, {}, self_obj);
      return fb;
    }
    if (!hasNonNullTarget(base_pts))
      return fb;

    const Id fn = index.by_site[site].function;
    std::set<Id> vals(fb.aliases.begin(), fb.aliases.end());
    if (fn < non_null_values_by_fn.size()) {
      for (Id cand : non_null_values_by_fn[fn])
        if (cand < upper.size() && upper[cand].intersectsNonNull(base_pts))
          vals.insert(cand);
    }
    if (fn < non_null_constants_by_fn.size()) {
      for (Id cand : non_null_constants_by_fn[fn])
        if (cand < upper.size() && upper[cand].intersectsNonNull(base_pts))
          vals.insert(cand);
    }
    fb.aliases.assign(vals.begin(), vals.end());

    std::set<Id> objs(fb.object_aliases.begin(), fb.object_aliases.end());
    for (Id o : non_null_ptr_mem_objects) {
      if (o < object_upper.size() &&
          object_upper[o].intersectsNonNull(base_pts))
        objs.insert(o);
    }
    fb.object_aliases.assign(objs.begin(), objs.end());
    finalizeAliasResult(fb, base_pts, site, point, {}, self_obj);
    return fb;
  }

  QueryResult queryObject(Id object, Id site, const Context *context_path,
                          Point point,
                          ContextMatch match = ContextMatch::Full) {
    if (object >= program.objects.size() || !isMemory(program.objects[object]))
      throw std::invalid_argument("unknown or non-memory query object");
    validateSiteAndContext(site, context_path);
    ClusterSolver &solver = ensureObjectSolver(object);
    QueryResult res =
        solver.queryObject(object, site, context_path, point, match);
    mergeStatsDelta(merged_object_solver_stats[object], solver.statistics());
    if (res.status == QueryStatus::ResourceLimit) {
      ++stats.fallback_queries;
      return {fallbackObjectPointsTo(object), QueryStatus::ResourceLimit};
    }
    return res;
  }

  static bool hasNonNullTarget(const PointsToSet &pts) {
    return pts.hasNonNull();
  }

  static bool memoryIntersects(const PointsToSet &a, const PointsToSet &b) {
    return a.intersectsNonNull(b);
  }

  void finalizeAliasResult(AliasResult &res, const PointsToSet &base_pts,
                           Id site, Point point,
                           const std::vector<const ClusterSolver *> &solvers_used,
                           Id self_obj) const {
    if (!res.reachable())
      return;
    const bool base_has_non_null =
        res.may_alias_all || hasNonNullTarget(base_pts);
    std::set<Id> merged_vals(res.aliases.begin(), res.aliases.end());
    std::set<Id> merged_objs(res.object_aliases.begin(),
                             res.object_aliases.end());
    if (self_obj != INVALID)
      merged_objs.insert(self_obj);
    if (!base_has_non_null) {
      res.aliases.assign(merged_vals.begin(), merged_vals.end());
      res.object_aliases.assign(merged_objs.begin(), merged_objs.end());
      return;
    }

    const InstLocation &qloc = index.by_site[site];
    const Id fn = qloc.function;

    if (res.may_alias_all) {
      // Full symmetry when `may_alias_all == true`: include all locally
      // reachable non-null-capable SSA values and constants referenced in `fn`.
      const Function &func = program.functions[fn];
      std::vector<bool> can_reach_strict(func.blocks.size(), false);
      {
        std::vector<Id> stack;
        for (const auto &[pblock, eidx] : index.pred_edges[fn][qloc.block]) {
          (void)eidx;
          if (!can_reach_strict[pblock]) {
            can_reach_strict[pblock] = true;
            stack.push_back(pblock);
          }
        }
        while (!stack.empty()) {
          const Id cur = stack.back();
          stack.pop_back();
          for (const auto &[pblock, eidx] : index.pred_edges[fn][cur]) {
            (void)eidx;
            if (!can_reach_strict[pblock]) {
              can_reach_strict[pblock] = true;
              stack.push_back(pblock);
            }
          }
        }
      }
      auto isDefReachable = [&](Id cand) -> bool {
        for (Id param : func.parameters)
          if (param == cand)
            return true;
        for (Id dsite : index.defining_sites[cand]) {
          const InstLocation &dloc = index.by_site[dsite];
          if (dloc.function != fn)
            continue;
          if (can_reach_strict[dloc.block])
            return true;
          if (dloc.block == qloc.block &&
              (dloc.index < qloc.index ||
               (dloc.index == qloc.index && point == Point::After)))
            return true;
        }
        for (const auto &[pf, pedge] : index.phi_defs[cand]) {
          if (pf != fn)
            continue;
          const Id target =
              func.blocks[pedge.first].successors[pedge.second].target;
          if (target == qloc.block || can_reach_strict[target])
            return true;
        }
        return false;
      };
      if (fn < non_null_values_by_fn.size()) {
        for (Id v : non_null_values_by_fn[fn])
          if (isDefReachable(v))
            merged_vals.insert(v);
      }
      if (fn < non_null_constants_by_fn.size()) {
        for (Id c : non_null_constants_by_fn[fn])
          merged_vals.insert(c);
      }
    }

    if (hierarchy.top_component != INVALID &&
        hierarchy.top_component < hierarchy.values.size()) {
      for (Id tv : hierarchy.values[hierarchy.top_component]) {
        const Value &vinfo = program.values[tv];
        if (!vinfo.constant && vinfo.function != INVALID &&
            vinfo.function != fn)
          continue;
        merged_vals.insert(tv);
      }
    }
    if (fn < top_upper_values_by_fn.size()) {
      for (Id tv : top_upper_values_by_fn[fn])
        merged_vals.insert(tv);
    }
    if (fn < empty_upper_values_by_fn.size()) {
      for (Id ev : empty_upper_values_by_fn[fn])
        merged_vals.insert(ev);
    }

    // Complete memory-object aliases for out-of-slice pointer objects (e.g.,
    // never-stored globals initialized to an aliased target, or untracked
    // objects whose contents may be Top).
    auto isTrackedBySolver = [&](Id obj) -> bool {
      for (const ClusterSolver *s : solvers_used)
        if (s && s->tracksObject(obj))
          return true;
      return false;
    };
    if (res.may_alias_all) {
      for (Id o : non_null_ptr_mem_objects)
        if (!isTrackedBySolver(o))
          merged_objs.insert(o);
    } else {
      for (Id t : base_pts.objects()) {
        if (t == NULL_OBJECT || t >= hierarchy.object_components.size())
          continue;
        const Id tcomp = hierarchy.object_components[t];
        if (tcomp >= ptr_mem_objs_by_target_comp.size())
          continue;
        for (Id cand : ptr_mem_objs_by_target_comp[tcomp]) {
          if (isTrackedBySolver(cand))
            continue;
          const Object &oinfo = program.objects[cand];
          if (oinfo.kind == ObjectKind::Global &&
              memoryIntersects(oinfo.initial, base_pts))
            merged_objs.insert(cand);
        }
      }
      if (hierarchy.top_component != INVALID &&
          hierarchy.top_component < ptr_mem_objs_by_target_comp.size()) {
        for (Id cand : ptr_mem_objs_by_target_comp[hierarchy.top_component]) {
          if (!isTrackedBySolver(cand))
            merged_objs.insert(cand);
        }
      }
    }

    res.aliases.assign(merged_vals.begin(), merged_vals.end());
    res.object_aliases.assign(merged_objs.begin(), merged_objs.end());
  }

  AliasResult queryObjectAliases(Id object, Id site,
                                 const Context *context_path, Point point,
                                 ContextMatch match = ContextMatch::Full) {
    if (object >= program.objects.size() || !isMemory(program.objects[object]))
      throw std::invalid_argument("unknown or non-memory query object");
    validateSiteAndContext(site, context_path);
    ClusterSolver &solver = ensureObjectSolver(object);
    AliasResult res =
        solver.queryObjectAliases(object, site, context_path, point, match);
    mergeStatsDelta(merged_object_solver_stats[object], solver.statistics());
    if (res.status == QueryStatus::ResourceLimit) {
      ++stats.fallback_queries;
      return buildFallbackAliases(fallbackObjectPointsTo(object), INVALID,
                                  object, site, point);
    }
    if (res.reachable()) {
      const QueryResult base =
          solver.queryObject(object, site, context_path, point, match);
      if (base.reachable())
        finalizeAliasResult(res, base.points_to, site, point, {&solver},
                            object);
    }
    return res;
  }

  bool mustAlias(Id lhs, Id rhs, Id site, const Context *context_path,
                 ContextMatch match = ContextMatch::Full) {
    validateQueryArgs(lhs, site, context_path);
    validateQueryArgs(rhs, site, context_path);
    QueryResult a = query(lhs, site, context_path, Point::Before, match);
    if (a.status != QueryStatus::Complete)
      return false;
    if (program.values[lhs].constant && program.values[lhs].initial.isTop())
      return false;
    if (lhs == rhs)
      return true;
    QueryResult b = query(rhs, site, context_path, Point::Before, match);
    if (b.status != QueryStatus::Complete)
      return false;
    if (program.values[rhs].constant && program.values[rhs].initial.isTop())
      return false;
    // Preserve `mustAlias => mayAlias`: strictly-NULL pointers have no
    // dereferenceable memory object in common.
    auto hasNonNull = [](const PointsToSet &pts) -> bool {
      if (pts.isTop())
        return true;
      for (Id o : pts.objects())
        if (o != NULL_OBJECT)
          return true;
      return false;
    };
    if (!hasNonNull(a.points_to) || !hasNonNull(b.points_to))
      return false;
    if (!a.points_to.isTop() && !b.points_to.isTop() &&
        a.points_to.size() == 1 && a.points_to == b.points_to) {
      const Id obj = *a.points_to.begin();
      if (obj != NULL_OBJECT && obj < index.exact_address_singleton.size() &&
          index.exact_address_singleton[obj])
        return true;
    }
    // Direct SSA copy / constant-offset field identity: if both `lhs` and `rhs`
    // trace inside `fn` through dominating single-def non-PHI Copy/Field chains
    // to the exact same invariant non-constant SSA root + byte offset (e.g.,
    // `q = bitcast p` on a non-singleton heap allocation), they must hold
    // identical runtime addresses!
    const InstLocation &qloc = index.by_site[site];
    const Id fn = qloc.function;
    const auto &fn_params = program.functions[fn].parameters;
    auto isInvariantAtSite = [&](Id v) -> bool {
      if (v == INVALID || v >= program.values.size())
        return false;
      const Value &vi = program.values[v];
      if (vi.constant)
        return false;
      if (vi.function != fn || !index.phi_defs[v].empty())
        return false;
      const auto &defs = index.defining_sites[v];
      if (defs.empty())
        return std::find(fn_params.begin(), fn_params.end(), v) !=
               fn_params.end();
      if (defs.size() != 1)
        return false;
      const InstLocation &dloc = index.by_site[defs[0]];
      if (dloc.function != fn || index.in_loop_block[fn][dloc.block])
        return false;
      return (dloc.block == qloc.block && dloc.index < qloc.index) ||
             (dloc.block != qloc.block &&
              index.blockDominates(fn, dloc.block, qloc.block));
    };
    auto resolveDirectSSARoot = [&](Id v, std::int64_t &off) -> Id {
      off = 0;
      std::set<Id> seen;
      while (v != INVALID && seen.insert(v).second) {
        if (!isInvariantAtSite(v))
          return INVALID;
        const auto &defs = index.defining_sites[v];
        if (defs.size() != 1)
          return v;
        const Instruction &di = *index.by_site[defs[0]].inst;
        if (di.opcode == Opcode::Copy && !di.operands.empty() &&
            di.operands[0] != INVALID) {
          v = di.operands[0];
          continue;
        }
        if (di.opcode == Opcode::Field && di.field_known &&
            di.field_array_stride == 0 && !di.operands.empty() &&
            di.operands[0] != INVALID) {
          const std::int64_t delta = di.field_offset;
          if ((delta > 0 &&
               off > std::numeric_limits<std::int64_t>::max() - delta) ||
              (delta < 0 &&
               off < std::numeric_limits<std::int64_t>::min() - delta))
            return INVALID;
          off += delta;
          v = di.operands[0];
          continue;
        }
        return v;
      }
      return INVALID;
    };
    std::int64_t off_l = 0, off_r = 0;
    const Id root_l = resolveDirectSSARoot(lhs, off_l);
    const Id root_r = resolveDirectSSARoot(rhs, off_r);
    if (root_l != INVALID && root_l == root_r && off_l == off_r)
      return true;

    // Check shared cluster solver for symbolic must-alias (e.g., copy of a
    // non-singleton heap pointer within the same cluster).
    const std::vector<std::size_t> lhs_c = selectClusters(lhs);
    const std::vector<std::size_t> rhs_c = selectClusters(rhs);
    for (std::size_t c : lhs_c) {
      if (std::binary_search(rhs_c.begin(), rhs_c.end(), c)) {
        ensureSolvers({c});
        const bool res =
            solvers[c]->mustAlias(lhs, rhs, site, context_path, match);
        syncSolverStatistics(c);
        if (res)
          return true;
      }
    }
    return false;
  }

  std::vector<std::size_t> selectClusters(Id value) const {
    std::vector<std::size_t> selected;
    for (std::size_t c = 0; c < cover.size(); ++c) {
      if (std::binary_search(cover[c].begin(), cover[c].end(), value))
        selected.push_back(c);
    }
    return selected;
  }

  QueryResult query(Id value, Id site, const Context *context_path, Point point,
                    ContextMatch match = ContextMatch::Full) {
    validateQueryArgs(value, site, context_path);
    const std::vector<std::size_t> selected = selectClusters(value);
    ensureSolvers(selected);
    QueryResult result;
    for (std::size_t c : selected) {
      QueryResult part =
          solvers[c]->query(value, site, context_path, point, match);
      syncSolverStatistics(c);
      if (part.status == QueryStatus::ResourceLimit) {
        ++stats.fallback_queries;
        return {fallbackPointsTo(value), QueryStatus::ResourceLimit};
      }
      if (part.reachable()) {
        result.status = QueryStatus::Complete;
        result.points_to.join(part.points_to);
      }
    }
    return result;
  }

  AliasResult queryAliases(Id value, Id site, const Context *context_path,
                           Point point,
                           ContextMatch match = ContextMatch::Full) {
    validateQueryArgs(value, site, context_path);
    const std::vector<std::size_t> selected = selectClusters(value);
    ensureSolvers(selected);
    AliasResult result;
    std::set<Id> merged;
    std::set<Id> merged_objs;
    std::vector<const ClusterSolver *> used_solvers;
    used_solvers.reserve(selected.size());
    for (std::size_t c : selected) {
      used_solvers.push_back(solvers[c].get());
      AliasResult part =
          solvers[c]->queryAliases(value, site, context_path, point, match);
      syncSolverStatistics(c);
      if (part.status == QueryStatus::ResourceLimit) {
        ++stats.fallback_queries;
        return buildFallbackAliases(fallbackPointsTo(value), value, INVALID,
                                    site, point);
      }
      if (part.reachable()) {
        result.status = QueryStatus::Complete;
        result.may_alias_all |= part.may_alias_all;
        merged.insert(part.aliases.begin(), part.aliases.end());
        merged_objs.insert(part.object_aliases.begin(),
                           part.object_aliases.end());
      }
    }
    result.aliases.assign(merged.begin(), merged.end());
    result.object_aliases.assign(merged_objs.begin(), merged_objs.end());
    if (result.reachable()) {
      const PointsToSet base_pts =
          result.may_alias_all
              ? PointsToSet::top()
              : query(value, site, context_path, point, match).points_to;
      // If `value` came from a dedicated top-upper cluster and the FSCS solver
      // refined it down to a concrete non-empty set, include any local values
      // from concrete object clusters whose points-to sets intersect `base_pts`.
      if (!result.may_alias_all && upper[value].isTop() &&
          hasNonNullTarget(base_pts)) {
        const Id fn = index.by_site[site].function;
        auto checkCand = [&](Id cand) {
          if (merged.count(cand) || !upper[cand].intersectsNonNull(base_pts))
            return;
          QueryResult cq = query(cand, site, context_path, point, match);
          if (cq.reachable() && memoryIntersects(cq.points_to, base_pts))
            merged.insert(cand);
        };
        if (fn < non_null_values_by_fn.size())
          for (Id cand : non_null_values_by_fn[fn])
            checkCand(cand);
        if (fn < non_null_constants_by_fn.size())
          for (Id cand : non_null_constants_by_fn[fn])
            checkCand(cand);
        result.aliases.assign(merged.begin(), merged.end());
      }
      finalizeAliasResult(result, base_pts, site, point, used_solvers, INVALID);
    }
    return result;
  }

  bool byteRangesOverlap(Id o1, std::uint64_t sz1, Id o2,
                         std::uint64_t sz2) const {
    if (o1 == NULL_OBJECT || o2 == NULL_OBJECT)
      return false;
    if (o1 == o2)
      return true;
    if (o1 < 2 || o2 < 2 || o1 >= program.objects.size() ||
        o2 >= program.objects.size())
      return true;
    const Id b1 = program.baseOf(o1);
    const Id b2 = program.baseOf(o2);
    if (b1 != b2)
      return false;
    const Object &base_info = program.objects[b1];
    if (!base_info.field_sensitive)
      return true;
    if (b1 < index.has_unaligned_interior.size() &&
        index.has_unaligned_interior[b1])
      return true;
    if (sz1 == 0 && sz2 == 0)
      return false;
    auto spanOf = [&](Id o, std::uint64_t req_sz) -> std::uint64_t {
      if (req_sz > 0)
        return req_sz;
      const Object &oi = program.objects[o];
      if (o == b1)
        return 0; // Unspecified size at base covers entire aggregate.
      return oi.size_bytes > 0 ? oi.size_bytes : oi.bytes;
    };
    const std::uint64_t s1 = spanOf(o1, sz1);
    const std::uint64_t s2 = spanOf(o2, sz2);
    if (s1 == 0 || s2 == 0)
      return true;
    const std::uint64_t off1 = program.objects[o1].offset;
    const std::uint64_t off2 = program.objects[o2].offset;
    // Direct byte-interval overlap [off1, off1+s1) vs [off2, off2+s2).
    if (off1 < off2 + s2 && off2 < off1 + s1)
      return true;
    // Check folded array regions: an access wider than the element stride or
    // spanning across elements can overlap folded sibling fields.
    for (const ArrayRegion &ar : base_info.array_regions) {
      if (ar.stride == 0)
        continue;
      const bool in1 = (off1 >= ar.begin && off1 < ar.end);
      const bool in2 = (off2 >= ar.begin && off2 < ar.end);
      if (in1 && in2) {
        if (s1 >= ar.stride || s2 >= ar.stride)
          return true;
        const std::uint64_t r1 = (off1 - ar.begin) % ar.stride;
        const std::uint64_t r2 = (off2 - ar.begin) % ar.stride;
        if (r1 < r2 + s2 && r2 < r1 + s1)
          return true;
        if (r1 + s1 > ar.stride && r2 < (r1 + s1 - ar.stride))
          return true;
        if (r2 + s2 > ar.stride && r1 < (r2 + s2 - ar.stride))
          return true;
      }
    }
    return false;
  }

  AliasKind queryAliasImpl(Id lhs, std::uint64_t lhs_size, Id lhs_site,
                           const Context *lhs_ctx, Point lhs_point,
                           ContextMatch lhs_match, Id rhs,
                           std::uint64_t rhs_size, Id rhs_site,
                           const Context *rhs_ctx, Point rhs_point,
                           ContextMatch rhs_match) {
    QueryResult a = query(lhs, lhs_site, lhs_ctx, lhs_point, lhs_match);
    QueryResult b = query(rhs, rhs_site, rhs_ctx, rhs_point, rhs_match);
    if (!a.reachable() || !b.reachable())
      return AliasKind::MayAlias;

    const bool same_context =
        (lhs_ctx == nullptr && rhs_ctx == nullptr) ||
        (lhs_ctx != nullptr && rhs_ctx != nullptr && lhs_match == rhs_match &&
         *lhs_ctx == *rhs_ctx);
    const bool same_point =
        (lhs_site == rhs_site && lhs_point == rhs_point && same_context);

    if (same_point && lhs_point == Point::Before &&
        mustAlias(lhs, rhs, lhs_site, lhs_ctx, lhs_match)) {
      if (lhs_size == rhs_size)
        return AliasKind::MustAlias;
      if (lhs_size != 0 && rhs_size != 0)
        return AliasKind::PartialAlias;
      return AliasKind::MayAlias;
    }

    if (a.points_to.isTop())
      return hasNonNullTarget(b.points_to) ? AliasKind::MayAlias
                                           : AliasKind::NoAlias;
    if (b.points_to.isTop())
      return hasNonNullTarget(a.points_to) ? AliasKind::MayAlias
                                           : AliasKind::NoAlias;

    if (a.points_to.size() == 1 && b.points_to.size() == 1) {
      const Id o1 = *a.points_to.begin();
      const Id o2 = *b.points_to.begin();
      if (o1 == NULL_OBJECT || o2 == NULL_OBJECT)
        return AliasKind::NoAlias;
      const bool s1 = (o1 < index.exact_address_singleton.size() &&
                       index.exact_address_singleton[o1]);
      const bool s2 = (o2 < index.exact_address_singleton.size() &&
                       index.exact_address_singleton[o2]);
      if (o1 == o2 && s1) {
        if (lhs_size == rhs_size)
          return AliasKind::MustAlias;
        if (lhs_size != 0 && rhs_size != 0)
          return AliasKind::PartialAlias;
        return AliasKind::MayAlias;
      }
      if (o1 != o2 && s1 && s2 && program.baseOf(o1) == program.baseOf(o2) &&
          lhs_size > 0 && rhs_size > 0) {
        const std::uint64_t off1 = program.objects[o1].offset;
        const std::uint64_t off2 = program.objects[o2].offset;
        if (off1 < off2 + rhs_size && off2 < off1 + lhs_size)
          return AliasKind::PartialAlias;
      }
    }

    for (Id o1 : a.points_to.objects()) {
      if (o1 == NULL_OBJECT)
        continue;
      for (Id o2 : b.points_to.objects()) {
        if (byteRangesOverlap(o1, lhs_size, o2, rhs_size))
          return AliasKind::MayAlias;
      }
    }
    return AliasKind::NoAlias;
  }

  bool mayAliasCross(Id lhs, Id lhs_site, const Context *lhs_ctx,
                     Point lhs_point, ContextMatch lhs_match, Id rhs,
                     Id rhs_site, const Context *rhs_ctx, Point rhs_point,
                     ContextMatch rhs_match) {
    QueryResult a = query(lhs, lhs_site, lhs_ctx, lhs_point, lhs_match);
    QueryResult b = query(rhs, rhs_site, rhs_ctx, rhs_point, rhs_match);
    if (!a.reachable() || !b.reachable())
      return true;
    const bool same_context =
        (lhs_ctx == nullptr && rhs_ctx == nullptr) ||
        (lhs_ctx != nullptr && rhs_ctx != nullptr && lhs_match == rhs_match &&
         *lhs_ctx == *rhs_ctx);
    if (lhs == rhs && lhs_site == rhs_site && lhs_point == rhs_point &&
        same_context)
      return true;
    return memoryIntersects(a.points_to, b.points_to);
  }

  bool mayNullImpl(Id value, Id site, const Context *context_path, Point point,
                   ContextMatch match) {
    QueryResult r = query(value, site, context_path, point, match);
    if (!r.reachable())
      return false;
    if (r.status == QueryStatus::ResourceLimit || r.points_to.isTop())
      return true;
    return r.points_to.contains(NULL_OBJECT);
  }

  CallTargets callTargetsImpl(Id call_site, const Context *context_path,
                              ContextMatch match) {
    validateSiteAndContext(call_site, context_path);
    auto sit = sites.find(call_site);
    if (sit == sites.end() || !sit->second.second)
      throw std::invalid_argument("callTargets requires a Call site");
    const Instruction &ci = *index.by_site[call_site].inst;
    CallTargets out;
    if (ci.callee != INVALID) {
      if (!reachability.reachable_site[call_site]) {
        out.status = QueryStatus::Unreachable;
        return out;
      }
      if (context_path != nullptr &&
          !(match == ContextMatch::Suffix && context_path->empty())) {
        // Validate the explicit context chain reaches call_site's function.
        const Id target_fn = index.by_site[call_site].function;
        Id cur = (match == ContextMatch::Suffix)
                     ? index.by_site[(*context_path)[0]].function
                     : program.entry;
        bool ok = reachability.reachable_function[cur];
        for (std::size_t i = 0; ok && i < context_path->size(); ++i) {
          const Id cs = (*context_path)[i];
          const InstLocation &cl = index.by_site[cs];
          if (cl.function != cur || !reachability.reachable_site[cs]) {
            ok = false;
            break;
          }
          const Id next = (i + 1 < context_path->size())
                              ? index.by_site[(*context_path)[i + 1]].function
                              : target_fn;
          const Targets ct = callees(
              program, *cl.inst,
              cl.inst->callee == INVALID
                  ? valueOf(program, upper, cl.inst->indirect_target)
                  : PointsToSet{});
          if (!ct.functions.count(next)) {
            ok = false;
            break;
          }
          cur = next;
        }
        if (!ok || cur != target_fn) {
          out.status = QueryStatus::Unreachable;
          return out;
        }
      }
      out.functions.push_back(ci.callee);
      out.status = QueryStatus::Complete;
      return out;
    }
    if (ci.indirect_target == INVALID) {
      out.may_call_unknown = true;
      out.status = reachability.reachable_site[call_site]
                       ? QueryStatus::Complete
                       : QueryStatus::Unreachable;
      return out;
    }
    QueryResult qr =
        query(ci.indirect_target, call_site, context_path, Point::Before, match);
    out.status = qr.status;
    if (!qr.reachable())
      return out;
    if (qr.status == QueryStatus::ResourceLimit) {
      out.may_call_unknown = true;
      return out;
    }
    const Targets t = callees(program, ci, qr.points_to);
    out.functions.assign(t.functions.begin(), t.functions.end());
    out.may_call_unknown = t.opaque;
    return out;
  }

  ModRefResult functionModRefImpl(Id root_fn) {
    if (root_fn >= program.functions.size())
      throw std::invalid_argument("unknown query function");
    ModRefResult out;
    auto addObjs = [&](PointsToSet &dst, bool &unknown_flag,
                       const PointsToSet &pts, std::uint64_t bytes) {
      if (pts.isTop()) {
        unknown_flag = true;
        dst = PointsToSet::top();
        return;
      }
      for (Id o : pts.objects()) {
        if (o == NULL_OBJECT || o < 2 || o >= program.objects.size() ||
            !isMemory(program.objects[o]))
          continue;
        dst.insert(o);
        for (Id fo : program.overlappingFields(o, bytes))
          if (fo >= 2 && isMemory(program.objects[fo]))
            dst.insert(fo);
      }
    };

    if (program.functions[root_fn].external) {
      out.may_ref_unknown = true;
      out.referenced = PointsToSet::top();
      if (program.functions[root_fn].writes_memory) {
        out.may_mod_unknown = true;
        out.modified = PointsToSet::top();
      }
      return out;
    }

    std::vector<bool> visited(program.functions.size(), false);
    std::vector<Id> work{root_fn};
    visited[root_fn] = true;
    while (!work.empty()) {
      const Id f = work.back();
      work.pop_back();
      const Function &func = program.functions[f];
      if (func.external) {
        out.may_ref_unknown = true;
        out.referenced = PointsToSet::top();
        if (func.writes_memory) {
          out.may_mod_unknown = true;
          out.modified = PointsToSet::top();
        }
        continue;
      }
      for (const Block &block : func.blocks) {
        for (const Instruction &inst : block.instructions) {
          if (!reachability.reachable_site[inst.id])
            continue;
          switch (inst.opcode) {
          case Opcode::Load: {
            QueryResult qr =
                query(inst.operands[0], inst.id, nullptr, Point::Before);
            if (qr.reachable())
              addObjs(out.referenced, out.may_ref_unknown, qr.points_to,
                      inst.read_bytes);
            break;
          }
          case Opcode::Store: {
            QueryResult qr =
                query(inst.operands[0], inst.id, nullptr, Point::Before);
            if (qr.reachable())
              addObjs(out.modified, out.may_mod_unknown, qr.points_to,
                      inst.write_bytes);
            break;
          }
          case Opcode::MemCopy: {
            QueryResult qd =
                query(inst.operands[0], inst.id, nullptr, Point::Before);
            QueryResult qs =
                query(inst.operands[1], inst.id, nullptr, Point::Before);
            if (qd.reachable())
              addObjs(out.modified, out.may_mod_unknown, qd.points_to,
                      inst.write_bytes);
            if (qs.reachable())
              addObjs(out.referenced, out.may_ref_unknown, qs.points_to,
                      inst.write_bytes);
            break;
          }
          case Opcode::Havoc: {
            if (inst.operands.empty()) {
              out.may_mod_unknown = true;
              out.modified = PointsToSet::top();
            } else {
              QueryResult qr =
                  query(inst.operands[0], inst.id, nullptr, Point::Before);
              if (qr.reachable())
                addObjs(out.modified, out.may_mod_unknown, qr.points_to,
                        inst.write_bytes);
            }
            break;
          }
          case Opcode::Call: {
            CallTargets ct =
                callTargetsImpl(inst.id, nullptr, ContextMatch::Full);
            if (!ct.reachable())
              break;
            if (ct.may_call_unknown || inst.opaque_alternative) {
              out.may_ref_unknown = true;
              out.referenced = PointsToSet::top();
              if (inst.writes_memory) {
                out.may_mod_unknown = true;
                out.modified = PointsToSet::top();
              }
            }
            for (Id callee : ct.functions) {
              if (program.functions[callee].external) {
                out.may_ref_unknown = true;
                out.referenced = PointsToSet::top();
                if (inst.writes_memory &&
                    program.functions[callee].writes_memory) {
                  out.may_mod_unknown = true;
                  out.modified = PointsToSet::top();
                }
              } else if (!visited[callee]) {
                visited[callee] = true;
                work.push_back(callee);
              }
            }
            break;
          }
          default:
            break;
          }
        }
      }
    }
    return out;
  }

  ModRefResult callModRefImpl(Id call_site, const Context *context_path,
                              ContextMatch match) {
    CallTargets ct = callTargetsImpl(call_site, context_path, match);
    ModRefResult out;
    if (!ct.reachable())
      return out;
    const Instruction &ci = *index.by_site[call_site].inst;
    if (ct.may_call_unknown || ci.opaque_alternative) {
      out.may_ref_unknown = true;
      out.referenced = PointsToSet::top();
      if (ci.writes_memory) {
        out.may_mod_unknown = true;
        out.modified = PointsToSet::top();
      }
    }
    for (Id callee : ct.functions) {
      if (program.functions[callee].external) {
        out.may_ref_unknown = true;
        out.referenced = PointsToSet::top();
        if (ci.writes_memory && program.functions[callee].writes_memory) {
          out.may_mod_unknown = true;
          out.modified = PointsToSet::top();
        }
        continue;
      }
      ModRefResult sub = functionModRefImpl(callee);
      out.modified.join(sub.modified);
      out.referenced.join(sub.referenced);
      out.may_mod_unknown |= sub.may_mod_unknown;
      out.may_ref_unknown |= sub.may_ref_unknown;
    }
    return out;
  }

  ReversePointsToResult reversePointsToImpl(Id object) const {
    if (object >= program.objects.size())
      throw std::invalid_argument("unknown query object");
    ReversePointsToResult out;
    for (Id v = 0; v < program.values.size(); ++v) {
      const PointsToSet &pts =
          program.values[v].constant ? program.values[v].initial : upper[v];
      if (pts.isTop()) {
        out.may_be_pointed_by_unknown = true;
        out.values.push_back(v);
      } else if (pts.contains(object)) {
        out.values.push_back(v);
      }
    }
    if (object != NULL_OBJECT && object < hierarchy.object_components.size()) {
      const Id tcomp = hierarchy.object_components[object];
      std::set<Id> objs;
      if (tcomp < ptr_mem_objs_by_target_comp.size()) {
        for (Id mo : ptr_mem_objs_by_target_comp[tcomp])
          objs.insert(mo);
      }
      if (hierarchy.top_component != INVALID &&
          hierarchy.top_component < ptr_mem_objs_by_target_comp.size()) {
        for (Id mo : ptr_mem_objs_by_target_comp[hierarchy.top_component]) {
          objs.insert(mo);
          out.may_be_pointed_by_unknown = true;
        }
      }
      out.memory_objects.assign(objs.begin(), objs.end());
    }
    return out;
  }
};

Analysis::Analysis(const Program &program, Options options)
    : m_impl(std::make_unique<Impl>(program, options)) {}
Analysis::~Analysis() = default;
Analysis::Analysis(Analysis &&) noexcept = default;
Analysis &Analysis::operator=(Analysis &&) noexcept = default;
QueryResult Analysis::pointsTo(Id value, Id site, const Context &context,
                               Point point, ContextMatch match) {
  return m_impl->query(value, site, &context, point, match);
}
QueryResult Analysis::pointsToAllContexts(Id value, Id site, Point point) {
  return m_impl->query(value, site, nullptr, point, ContextMatch::Full);
}
AliasResult Analysis::aliases(Id value, Id site, const Context &context,
                              Point point, ContextMatch match) {
  return m_impl->queryAliases(value, site, &context, point, match);
}
AliasResult Analysis::aliasesAllContexts(Id value, Id site, Point point) {
  return m_impl->queryAliases(value, site, nullptr, point, ContextMatch::Full);
}
QueryResult Analysis::objectPointsTo(Id object, Id site, const Context &context,
                                     Point point, ContextMatch match) {
  return m_impl->queryObject(object, site, &context, point, match);
}
QueryResult Analysis::objectPointsToAllContexts(Id object, Id site,
                                                Point point) {
  return m_impl->queryObject(object, site, nullptr, point, ContextMatch::Full);
}
AliasResult Analysis::objectAliases(Id object, Id site, const Context &context,
                                    Point point, ContextMatch match) {
  return m_impl->queryObjectAliases(object, site, &context, point, match);
}
AliasResult Analysis::objectAliasesAllContexts(Id object, Id site,
                                               Point point) {
  return m_impl->queryObjectAliases(object, site, nullptr, point,
                                    ContextMatch::Full);
}
void Analysis::precomputeAll() { m_impl->precomputeAll(); }
void Analysis::precomputeForValues(const std::vector<Id> &values) {
  m_impl->precomputeForValues(values);
}
void Analysis::precomputeClusters(
    const std::vector<std::size_t> &cluster_indices) {
  m_impl->precomputeClusters(cluster_indices);
}
std::vector<std::size_t> Analysis::clustersFor(Id value) const {
  if (value >= m_impl->program.values.size())
    throw std::invalid_argument("unknown query value");
  return m_impl->selectClusters(value);
}
bool Analysis::mayAlias(Id lhs, Id rhs, Id site, const Context &context,
                        ContextMatch match) {
  return m_impl->mayAliasCross(lhs, site, &context, Point::Before, match, rhs,
                               site, &context, Point::Before, match);
}
bool Analysis::mayAliasAcrossSites(Id lhs, Id lhs_site, Id rhs, Id rhs_site,
                                   Point lhs_point, Point rhs_point) {
  return m_impl->mayAliasCross(lhs, lhs_site, nullptr, lhs_point,
                               ContextMatch::Full, rhs, rhs_site, nullptr,
                               rhs_point, ContextMatch::Full);
}
bool Analysis::mayAliasAcrossSites(Id lhs, Id lhs_site,
                                   const Context &lhs_context, Id rhs,
                                   Id rhs_site, const Context &rhs_context,
                                   Point lhs_point, Point rhs_point,
                                   ContextMatch lhs_match,
                                   ContextMatch rhs_match) {
  return m_impl->mayAliasCross(lhs, lhs_site, &lhs_context, lhs_point,
                               lhs_match, rhs, rhs_site, &rhs_context,
                               rhs_point, rhs_match);
}
AliasKind Analysis::queryAlias(Id lhs, std::uint64_t lhs_size, Id lhs_site,
                               Id rhs, std::uint64_t rhs_size, Id rhs_site,
                               Point lhs_point, Point rhs_point) {
  return m_impl->queryAliasImpl(lhs, lhs_size, lhs_site, nullptr, lhs_point,
                                ContextMatch::Full, rhs, rhs_size, rhs_site,
                                nullptr, rhs_point, ContextMatch::Full);
}
AliasKind Analysis::queryAlias(Id lhs, std::uint64_t lhs_size, Id lhs_site,
                               const Context &lhs_context, Id rhs,
                               std::uint64_t rhs_size, Id rhs_site,
                               const Context &rhs_context, Point lhs_point,
                               Point rhs_point, ContextMatch lhs_match,
                               ContextMatch rhs_match) {
  return m_impl->queryAliasImpl(lhs, lhs_size, lhs_site, &lhs_context,
                                lhs_point, lhs_match, rhs, rhs_size, rhs_site,
                                &rhs_context, rhs_point, rhs_match);
}
bool Analysis::mustAlias(Id lhs, Id rhs, Id site, const Context &context,
                         ContextMatch match) {
  return m_impl->mustAlias(lhs, rhs, site, &context, match);
}
bool Analysis::mayNull(Id value, Id site, Point point) {
  return m_impl->mayNullImpl(value, site, nullptr, point, ContextMatch::Full);
}
bool Analysis::mayNull(Id value, Id site, const Context &context, Point point,
                       ContextMatch match) {
  return m_impl->mayNullImpl(value, site, &context, point, match);
}
CallTargets Analysis::callTargets(Id call_site) {
  return m_impl->callTargetsImpl(call_site, nullptr, ContextMatch::Full);
}
CallTargets Analysis::callTargets(Id call_site, const Context &context,
                                  ContextMatch match) {
  return m_impl->callTargetsImpl(call_site, &context, match);
}
std::vector<Id> Analysis::callersOf(Id function) const {
  if (function >= m_impl->program.functions.size())
    throw std::invalid_argument("unknown query function");
  std::vector<Id> callers = m_impl->index.incoming_calls[function];
  std::sort(callers.begin(), callers.end());
  callers.erase(std::unique(callers.begin(), callers.end()), callers.end());
  return callers;
}
bool Analysis::isRecursiveFunction(Id function) const {
  if (function >= m_impl->program.functions.size())
    throw std::invalid_argument("unknown query function");
  return m_impl->call_graph.scc_recursive[m_impl->call_graph.function_scc[function]];
}
ModRefResult Analysis::functionModRef(Id function) {
  return m_impl->functionModRefImpl(function);
}
ModRefResult Analysis::callModRef(Id call_site) {
  return m_impl->callModRefImpl(call_site, nullptr, ContextMatch::Full);
}
ModRefResult Analysis::callModRef(Id call_site, const Context &context,
                                  ContextMatch match) {
  return m_impl->callModRefImpl(call_site, &context, match);
}
ReversePointsToResult Analysis::reversePointsTo(Id object) {
  return m_impl->reversePointsToImpl(object);
}
const Object &Analysis::objectInfo(Id object) const {
  return m_impl->program.objects.at(object);
}
const Statistics &Analysis::statistics() const { return m_impl->stats; }
const std::vector<std::vector<Id>> &Analysis::clusters() const {
  return m_impl->cover;
}
const SteensgaardHierarchy &Analysis::hierarchy() const {
  return m_impl->hierarchy;
}

} // namespace bootstrap
} // namespace lotus
