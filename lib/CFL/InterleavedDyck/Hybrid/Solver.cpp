#include "CFL/InterleavedDyck/Hybrid/Solver.h"

#include "CFL/InterleavedDyck/SPDS/DyckReachability.h"
#include "CFL/InterleavedDyck/SPDS/Solver.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace lotus::cfl::interleaved_dyck::hybrid {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsedUs(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                            start)
          .count());
}

struct DemandFilter {
  PairSet pairs;
  std::unordered_set<Vertex> sources;
  std::unordered_set<Vertex> targets;

  explicit DemandFilter(const Demands &demands)
      : pairs(demands.pairs.begin(), demands.pairs.end()),
        sources(demands.sources.begin(), demands.sources.end()),
        targets(demands.targets.begin(), demands.targets.end()) {}

  bool contains(const Pair &pair) const {
    return pair.source != pair.target &&
           (pairs.count(pair) != 0U || sources.count(pair.source) != 0U ||
            targets.count(pair.target) != 0U);
  }
};

Graph retainMatchedLabelsOnce(const Graph &graph) {
  std::unordered_set<unsigned> par_open, par_close, brk_open, brk_close;
  for (const Edge &edge : graph.edges()) {
    switch (edge.label.kind) {
    case LabelKind::OpenParenthesis:
      par_open.insert(edge.label.id);
      break;
    case LabelKind::CloseParenthesis:
      par_close.insert(edge.label.id);
      break;
    case LabelKind::OpenBracket:
      brk_open.insert(edge.label.id);
      break;
    case LabelKind::CloseBracket:
      brk_close.insert(edge.label.id);
      break;
    case LabelKind::Neutral:
      break;
    }
  }
  std::unordered_set<unsigned> par_matched, brk_matched;
  for (unsigned id : par_open) {
    if (par_close.count(id) != 0U)
      par_matched.insert(id);
  }
  for (unsigned id : brk_open) {
    if (brk_close.count(id) != 0U)
      brk_matched.insert(id);
  }
  Graph out;
  for (const Edge &edge : graph.edges()) {
    const auto kind = edge.label.kind;
    if (kind == LabelKind::Neutral ||
        ((kind == LabelKind::OpenParenthesis ||
          kind == LabelKind::CloseParenthesis) &&
         par_matched.count(edge.label.id) != 0U) ||
        ((kind == LabelKind::OpenBracket || kind == LabelKind::CloseBracket) &&
         brk_matched.count(edge.label.id) != 0U)) {
      out.addEdge(edge.source, edge.target, edge.label);
    }
  }
  return out;
}

Graph retainMatchedLabelsFixpoint(Graph graph) {
  while (!graph.edges().empty()) {
    Graph next = retainMatchedLabelsOnce(graph);
    if (next.edges().size() == graph.edges().size())
      break;
    graph = std::move(next);
  }
  return graph;
}

PairSet setDifference(const PairSet &a, const PairSet &b) {
  PairSet out;
  for (const Pair &p : a) {
    if (p.source != p.target && b.count(p) == 0U)
      out.insert(p);
  }
  return out;
}

void addAffineStats(affine::Statistics &dst, const affine::Statistics &src) {
  dst.saturation.states += src.saturation.states;
  dst.saturation.transitions += src.saturation.transitions;
  dst.saturation.updates += src.saturation.updates;
  dst.saturation.processed += src.saturation.processed;
  dst.saturation.rules += src.saturation.rules;
  dst.saturation.setup_microseconds += src.saturation.setup_microseconds;
  dst.saturation.saturation_microseconds +=
      src.saturation.saturation_microseconds;
  dst.saturation.readout_microseconds += src.saturation.readout_microseconds;
  dst.saturation.projection_microseconds +=
      src.saturation.projection_microseconds;
  dst.compiled_rules += src.compiled_rules;
  dst.prepared_weights += src.prepared_weights;
  dst.maximum_affine_rank =
      std::max(dst.maximum_affine_rank, src.maximum_affine_rank);
  dst.algebra.matrix_products += src.algebra.matrix_products;
  dst.algebra.basis_reductions += src.algebra.basis_reductions;
  dst.algebra.basis_insertions += src.algebra.basis_insertions;
  dst.algebra.cow_detaches += src.algebra.cow_detaches;
  dst.algebra.intersection_tests += src.algebra.intersection_tests;
  dst.algebra.intersection_fast_paths += src.algebra.intersection_fast_paths;
  dst.algebra.intersection_us += src.algebra.intersection_us;
}

PairSet runStage1SPDS(const Graph &graph, const Demands *raw_demands,
                      const DemandFilter *filter) {
  PairSet out;
  if (graph.edges().empty())
    return out;

  const auto collect = [&](const PairSet &pairs) {
    for (const Pair &p : pairs) {
      if (p.source != p.target && (!filter || filter->contains(p)))
        out.insert(p);
    }
  };
  if (!raw_demands) {
    collect(spds::filterInterleavedBooleanAll(graph, false));
    return out;
  }
  std::vector<Pair> valid_pairs;
  valid_pairs.reserve(raw_demands->pairs.size());
  for (const Pair &p : raw_demands->pairs) {
    if (p.source != p.target && graph.containsVertex(p.source) &&
        graph.containsVertex(p.target))
      valid_pairs.push_back(p);
  }
  if (!valid_pairs.empty())
    collect(spds::filterInterleavedBooleanDemands(graph, valid_pairs, false));

  for (Vertex s : raw_demands->sources) {
    if (!graph.containsVertex(s))
      continue;
    const auto brk_targets = spds::queryDyckReachableFromAnchor(
        graph, s, spds::Direction::Post, spds::DyckProjectionKind::Brackets);
    if (brk_targets.empty())
      continue;
    const auto par_targets = spds::queryDyckReachableFromAnchor(
        graph, s, spds::Direction::Post, spds::DyckProjectionKind::Parentheses);
    std::unordered_set<Vertex> par_set(par_targets.begin(), par_targets.end());
    for (Vertex t : brk_targets) {
      if (t != s && par_set.count(t) != 0U) {
        const Pair p{s, t};
        if (!filter || filter->contains(p))
          out.insert(p);
      }
    }
  }
  for (Vertex t : raw_demands->targets) {
    if (!graph.containsVertex(t))
      continue;
    const auto brk_sources = spds::queryDyckReachableFromAnchor(
        graph, t, spds::Direction::Pre, spds::DyckProjectionKind::Brackets);
    if (brk_sources.empty())
      continue;
    const auto par_sources = spds::queryDyckReachableFromAnchor(
        graph, t, spds::Direction::Pre, spds::DyckProjectionKind::Parentheses);
    std::unordered_set<Vertex> par_set(par_sources.begin(), par_sources.end());
    for (Vertex s : brk_sources) {
      if (s != t && par_set.count(s) != 0U) {
        const Pair p{s, t};
        if (!filter || filter->contains(p))
          out.insert(p);
      }
    }
  }
  return out;
}

// Demand-driven certified union-Dyck lower bound via condensed shared-RHS
// tabulation over the disjoint union of parentheses and brackets.
PairSet demandUnionDyckLowerBound(const Graph &graph,
                                  const PairSet &candidates) {
  if (candidates.empty() || graph.edges().empty())
    return {};
  const std::vector<Pair> cand_vec(candidates.begin(), candidates.end());
  return spds::filterDyckReachableDemands(
      graph, cand_vec, spds::DyckProjectionKind::UnionDyck, false);
}

Result runHybridPipeline(const Graph &input, const Demands *raw_demands,
                         const Options &options) {
  if (options.parity_groups == 0)
    throw std::invalid_argument("hybrid parity_groups must be >= 1");

  const auto total_start = Clock::now();
  Result result;
  result.statistics.matrix_dimension =
      static_cast<std::size_t>(options.parity_groups) * 2U;

  std::optional<DemandFilter> filter;
  if (raw_demands)
    filter.emplace(*raw_demands);

  // Core 1 (Stage 1): Corridor slice + Boolean SPDS projection filter
  // (U1 = D_P \cap D_B)
  const auto slice_start = Clock::now();
  Graph graph = raw_demands
                    ? spds::sliceGraphForDemands(input, raw_demands->pairs,
                                                 raw_demands->sources,
                                                 raw_demands->targets)
                    : input;
  graph = retainMatchedLabelsFixpoint(std::move(graph));
  result.statistics.slice_vertices = graph.vertices().size();
  result.statistics.slice_edges = graph.edges().size();
  result.statistics.timings.slice_us = elapsedUs(slice_start);

  const auto s1_start = Clock::now();
  result.stage1_spds =
      runStage1SPDS(graph, raw_demands, filter ? &*filter : nullptr);
  result.statistics.timings.stage1_spds_us = elapsedUs(s1_start);

  if (result.stage1_spds.empty()) {
    result.statistics.short_circuited_after = 1;
    result.statistics.timings.total_us = elapsedUs(total_start);
    return result;
  }

  graph = retainMatchedLabelsFixpoint(
      spds::sliceGraphOnCandidatePaths(graph, result.stage1_spds));

  // Core 1 (Stage 2a): Demand union-Dyck certified lower bound (L1 = D_{P \cup B})
  const auto s2l_start = Clock::now();
  if (options.enable_lower) {
    result.certified_lower =
        demandUnionDyckLowerBound(graph, result.stage1_spds);
  }
  result.statistics.stage2_lower_count = result.certified_lower.size();
  result.statistics.timings.stage2_lower_us = elapsedUs(s2l_start);

  PairSet unresolved =
      setDifference(result.stage1_spds, result.certified_lower);
  if (unresolved.empty()) {
    result.stage3_affine = result.certified_lower;
    result.final_upper = result.certified_lower;
    result.statistics.short_circuited_after = 2;
    result.statistics.timings.total_us = elapsedUs(total_start);
    return result;
  }

  // Core 2 (Stage 3): Single-sided ParitySemiring AffineSPDS on unresolved
  // (U1 \ L1) + dead-edge feedback
  const auto s3_start = Clock::now();
  if (options.enable_affine && !unresolved.empty() && !graph.edges().empty()) {
    Graph aff_graph = retainMatchedLabelsFixpoint(
        spds::sliceGraphOnCandidatePaths(graph, unresolved));
    PairSet active_unresolved;
    for (const Pair &p : unresolved) {
      if (aff_graph.containsVertex(p.source) &&
          aff_graph.containsVertex(p.target))
        active_unresolved.insert(p);
    }
    unresolved = std::move(active_unresolved);

    if (unresolved.empty()) {
      result.stage3_affine = result.certified_lower;
      graph = retainMatchedLabelsFixpoint(
          spds::sliceGraphOnCandidatePaths(graph, result.stage3_affine));
    } else if (options.affine_max_edges == 0U ||
               aff_graph.edges().size() <= options.affine_max_edges) {
      result.statistics.affine_executed = true;
      affine::Options aff_opts;
      aff_opts.limits = options.affine_limits;
      affine::SingleSidedParityOptions ss_opts;
      ss_opts.groups = options.parity_groups;
      ss_opts.corridor_active_rehash = options.corridor_active_rehash;
      ss_opts.mode = affine::ComparisonMode::Joint;
      const affine::Solver aff_solver(aff_opts);

      try {
        for (unsigned round = 0; round <= options.affine_feedback_rounds;
             ++round) {
          std::vector<Pair> query_vec;
          query_vec.reserve(unresolved.size());
          for (const Pair &p : unresolved) {
            if (aff_graph.containsVertex(p.source) &&
                aff_graph.containsVertex(p.target))
              query_vec.push_back(p);
          }
          if (query_vec.empty()) {
            unresolved.clear();
            break;
          }
          auto aff_res = aff_solver.analyzeSingleSidedParityDemands(
              aff_graph, query_vec, ss_opts);
          addAffineStats(result.statistics.affine_stats, aff_res.statistics);
          result.statistics.max_affine_rank =
              std::max(result.statistics.max_affine_rank,
                       aff_res.statistics.maximum_affine_rank);

          PairSet next_unresolved;
          for (const Pair &p : aff_res.pairs)
            if (p.source != p.target)
              next_unresolved.insert(p);

          const bool pruned_any = next_unresolved.size() < unresolved.size();
          unresolved = std::move(next_unresolved);
          if (!pruned_any || unresolved.empty())
            break;

          Graph shrunken = retainMatchedLabelsFixpoint(
              spds::sliceGraphOnCandidatePaths(aff_graph, unresolved));
          if (shrunken.edges().size() == aff_graph.edges().size())
            break;
          aff_graph = std::move(shrunken);
        }
      } catch (const spds::ResourceLimit &) {
        // Sound fallback: retain current `unresolved` upper bound.
      }
      result.stage3_affine = result.certified_lower;
      result.stage3_affine.insert(unresolved.begin(), unresolved.end());
      graph = retainMatchedLabelsFixpoint(
          spds::sliceGraphOnCandidatePaths(graph, result.stage3_affine));
    } else {
      result.stage3_affine = result.certified_lower;
      result.stage3_affine.insert(unresolved.begin(), unresolved.end());
      graph = retainMatchedLabelsFixpoint(
          spds::sliceGraphOnCandidatePaths(graph, result.stage3_affine));
    }
  } else {
    result.stage3_affine = result.stage1_spds;
  }
  result.statistics.post_stage3_vertices = graph.vertices().size();
  result.statistics.post_stage3_edges = graph.edges().size();
  result.statistics.timings.stage3_affine_us = elapsedUs(s3_start);

  result.final_upper = result.stage3_affine;
  if (unresolved.empty())
    result.statistics.short_circuited_after = 3;
  result.statistics.timings.total_us = elapsedUs(total_start);
  return result;
}

} // namespace

Result Solver::analyze(const Graph &graph, const Options &options) const {
  return runHybridPipeline(graph, nullptr, options);
}

Result Solver::analyzeDemands(const Graph &graph, const Demands &demands,
                              const Options &options) const {
  return runHybridPipeline(graph, &demands, options);
}

} // namespace lotus::cfl::interleaved_dyck::hybrid
