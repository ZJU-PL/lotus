#include "Alias/InclusionBased/BootstrapAA/Engine.h"
#include "Alias/InclusionBased/BootstrapAA/IR/ProgramIndex.h"
#include "Alias/InclusionBased/BootstrapAA/Preprocessing/Partitioning.h"
#include "Alias/InclusionBased/BootstrapAA/Preprocessing/RelevantSlice.h"
#include "Alias/InclusionBased/BootstrapAA/Solver/ClusterSolver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
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
  SteensgaardHierarchy hierarchy;
  CallGraphInfo call_graph;
  ReachabilityInfo reachability;
  std::vector<std::vector<Id>> cover;
  std::vector<std::unique_ptr<ClusterSolver>> solvers;
  std::vector<Statistics> merged_solver_stats;
  std::map<Id, std::pair<Id, bool>> sites;

  Impl(const Program &p, Options o) : program(p), options(o), index(program) {
    const auto preprocessingStart = std::chrono::steady_clock::now();
    program.validate();
    forInstructions(program, [&](Id f, const Instruction &i) {
      sites.emplace(i.id, std::make_pair(f, i.opcode == Opcode::Call));
    });

    // Stage 1: Steensgaard partitioning & retained points-to hierarchy (§2.1).
    CoarseResult coarse = steensgaard(program);
    upper = coarse.upper;
    hierarchy = std::move(coarse.hierarchy);
    index.populateCallers(program, upper);
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
        if (refined[v].empty()) {
          out.push_back({v});
        } else if (refined[v].isTop()) {
          top_vals.push_back(v);
        } else {
          for (Id object : refined[v].objects())
            inverse[object].push_back(v);
        }
      }
      if (!top_vals.empty()) {
        inverse[UNKNOWN];
        for (auto &[obj, vec] : inverse) {
          vec.insert(vec.end(), top_vals.begin(), top_vals.end());
          std::sort(vec.begin(), vec.end());
          vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
        }
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
          Sets olf = oneLevelFlow(program, index, partition, coarse.upper);
          auto sub = buildInverseCover(partition, olf);
          std::size_t sub_largest = 0;
          for (const auto &c : sub)
            sub_largest = std::max(sub_largest, c.size());
          if (sub_largest < partition.size()) {
            for (Id v : partition)
              upper[v] = olf[v];
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
        Slice slice = relevant(program, index, coarse.upper, hierarchy,
                               reachability, partition, true);
        Sets refined = inclusion(program, index, slice);
        auto refinedCover = buildInverseCover(partition, refined);
        std::size_t refinedLargest = 0;
        for (const auto &cluster : refinedCover)
          refinedLargest = std::max(refinedLargest, cluster.size());
        if (!options.adaptive_andersen_threshold) {
          for (Id value : partition)
            upper[value] = refined[value];
          cover.insert(cover.end(), refinedCover.begin(), refinedCover.end());
          continue;
        }
        const long double retainedFraction =
            partition.empty() ? 0.0L
                              : static_cast<long double>(refinedLargest) /
                                    static_cast<long double>(partition.size());
        if (retainedFraction >= 0.75L) {
          ++stats.adaptive_refinement_rejections;
          cover.push_back(partition);
          threshold = std::max(threshold, partition.size());
        } else {
          for (Id value : partition)
            upper[value] = refined[value];
          cover.insert(cover.end(), refinedCover.begin(), refinedCover.end());
          if (retainedFraction <= 0.5L)
            threshold = std::max<std::size_t>(1, threshold * 3 / 4);
        }
      }
      stats.effective_andersen_threshold = threshold;
    }

    index.populateCallers(program, upper);
    call_graph = buildCallGraph(program, upper);
    reachability = computeReachability(program, index, upper);
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

  BuiltSolver buildSolver(std::size_t cluster) const {
    const auto started = std::chrono::steady_clock::now();
    BuiltSolver built;
    built.cluster = cluster;
    Slice slice = relevant(program, index, upper, hierarchy, reachability,
                           cover[cluster], options.enable_slicing);
    if (options.solver_mode == SolverMode::InputStateTabulation) {
      built.solver = createTabulationSolver(program, index, upper, call_graph,
                                            std::move(slice), options);
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

  void syncSolverStatistics(std::size_t cluster) {
    if (!solvers[cluster])
      return;
    const Statistics &cur = solvers[cluster]->statistics();
    Statistics &prev = merged_solver_stats[cluster];
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

  void validateQueryArgs(Id value, Id site, const Context *context_path) const {
    if (value >= program.values.size() || !sites.count(site))
      throw std::invalid_argument("unknown query value or instruction");
    if (context_path) {
      for (Id callsite : *context_path) {
        auto it = sites.find(callsite);
        if (it == sites.end() || !it->second.second)
          throw std::invalid_argument("context contains a non-call site");
      }
    }
  }

  std::vector<std::size_t> selectClusters(Id value) const {
    std::vector<std::size_t> selected;
    for (std::size_t c = 0; c < cover.size(); ++c) {
      if (std::binary_search(cover[c].begin(), cover[c].end(), value))
        selected.push_back(c);
    }
    return selected;
  }

  QueryResult query(Id value, Id site, const Context *context_path,
                    Point point) {
    validateQueryArgs(value, site, context_path);
    const std::vector<std::size_t> selected = selectClusters(value);
    ensureSolvers(selected);
    QueryResult result;
    for (std::size_t c : selected) {
      QueryResult part = solvers[c]->query(value, site, context_path, point);
      syncSolverStatistics(c);
      if (part.status == QueryStatus::ResourceLimit) {
        ++stats.fallback_queries;
        return part;
      }
      if (part.reachable()) {
        result.status = QueryStatus::Complete;
        result.points_to.join(part.points_to);
      }
    }
    return result;
  }

  AliasResult queryAliases(Id value, Id site, const Context *context_path,
                           Point point) {
    validateQueryArgs(value, site, context_path);
    const std::vector<std::size_t> selected = selectClusters(value);
    ensureSolvers(selected);
    AliasResult result;
    std::set<Id> merged;
    for (std::size_t c : selected) {
      AliasResult part =
          solvers[c]->queryAliases(value, site, context_path, point);
      syncSolverStatistics(c);
      if (part.status == QueryStatus::ResourceLimit) {
        ++stats.fallback_queries;
        return part;
      }
      if (part.reachable()) {
        result.status = QueryStatus::Complete;
        result.may_alias_all |= part.may_alias_all;
        merged.insert(part.aliases.begin(), part.aliases.end());
      }
    }
    if (result.reachable() && hierarchy.top_component != INVALID &&
        hierarchy.top_component < hierarchy.values.size()) {
      const Id fn = index.by_site[site].function;
      for (Id tv : hierarchy.values[hierarchy.top_component]) {
        const Value &vinfo = program.values[tv];
        if (!vinfo.constant && vinfo.function != INVALID &&
            vinfo.function != fn)
          continue;
        merged.insert(tv);
      }
    }
    result.aliases.assign(merged.begin(), merged.end());
    return result;
  }
};

Analysis::Analysis(const Program &program, Options options)
    : m_impl(std::make_unique<Impl>(program, options)) {}
Analysis::~Analysis() = default;
Analysis::Analysis(Analysis &&) noexcept = default;
Analysis &Analysis::operator=(Analysis &&) noexcept = default;
QueryResult Analysis::pointsTo(Id value, Id site, const Context &context,
                               Point point) {
  return m_impl->query(value, site, &context, point);
}
QueryResult Analysis::pointsToAllContexts(Id value, Id site, Point point) {
  return m_impl->query(value, site, nullptr, point);
}
AliasResult Analysis::aliases(Id value, Id site, const Context &context,
                              Point point) {
  return m_impl->queryAliases(value, site, &context, point);
}
AliasResult Analysis::aliasesAllContexts(Id value, Id site, Point point) {
  return m_impl->queryAliases(value, site, nullptr, point);
}
void Analysis::precomputeAll() { m_impl->precomputeAll(); }
bool Analysis::mayAlias(Id lhs, Id rhs, Id site, const Context &context) {
  QueryResult a = pointsTo(lhs, site, context),
              b = pointsTo(rhs, site, context);
  if (!a.reachable() || !b.reachable())
    return true;
  return a.points_to.intersects(b.points_to);
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
