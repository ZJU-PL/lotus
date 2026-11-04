#include "CFL/InterleavedDyck/Unary/Adaptive.h"

#include "CFL/InterleavedDyck/Core/DisjointSets.h"
#include "CFL/InterleavedDyck/Unary/Support.h"

#include <algorithm>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::unary {
namespace {

using namespace detail;
using DisjointSets = interleaved_dyck::detail::DisjointSets;

struct RootLabel {
  std::size_t component = 0;
  std::size_t residual_height = 0;
  bool operator==(const RootLabel &other) const {
    return component == other.component &&
           residual_height == other.residual_height;
  }
};

struct LabeledObject {
  RootLabel label;
  std::size_t object = 0;
};

template <bool CollectTiming, typename Operation>
auto measurePhase(std::uint64_t &target, Operation &&operation) {
  if constexpr (CollectTiming) {
    const auto begin = Clock::now();
    auto result = std::forward<Operation>(operation)();
    target += elapsed(begin);
    return result;
  } else {
    return std::forward<Operation>(operation)();
  }
}

template <bool CollectTiming>
void recordPhase(std::uint64_t &target, std::uint64_t duration) {
  if constexpr (CollectTiming)
    target += duration;
}

template <typename Key>
void countingSort(std::vector<LabeledObject> &values,
                  std::vector<LabeledObject> &scratch,
                  std::vector<std::size_t> &offsets, std::size_t base,
                  Key key) {
  offsets.assign(base, 0);
  for (const auto &value : values)
    ++offsets.at(key(value));
  std::size_t next = 0;
  for (auto &offset : offsets) {
    const auto count = offset;
    offset = next;
    next += count;
  }
  for (const auto &value : values)
    scratch[offsets[key(value)]++] = value;
  values.swap(scratch);
}

void sortVerticalLabels(std::vector<LabeledObject> &labels, std::size_t width,
                        std::size_t components, AdaptiveStats &stats) {
  std::vector<LabeledObject> scratch(labels.size());
  std::vector<std::size_t> offsets;
  offsets.reserve(std::max(width, components));
  stats.execution.peak_working_bytes = std::max(
      stats.execution.peak_working_bytes,
      (labels.capacity() + scratch.capacity()) * sizeof(LabeledObject) +
          offsets.capacity() * sizeof(std::size_t));
  countingSort(labels, scratch, offsets, width,
               [](const auto &v) { return v.label.residual_height; });
  countingSort(labels, scratch, offsets, components,
               [](const auto &v) { return v.label.component; });
  // Scratch and counting buckets are released before allocating the merge DSU.
}

template <bool CollectTiming>
std::vector<std::size_t> shallowComponents(const UnaryGraph &graph,
                                           std::size_t threshold,
                                           AdaptiveStats &stats) {
  const auto vertical_view = measurePhase<CollectTiming>(
      stats.phase_timing.vertical_construction_us,
      [&] { return LiftedCounterGraph(graph, threshold, true); });
  const auto width = vertical_view.width;
  const auto object_count = checkedAdd(graph.vertex_count, vertical_view.states,
                                       "adaptive merge objects");
  stats.threshold = std::max(stats.threshold, threshold);
  stats.vertical_control_states += vertical_view.states;
  stats.horizontal_control_states += vertical_view.states;
  stats.vertical_arcs += vertical_view.arcs;
  std::vector<LabeledObject> labels;
  std::size_t vertical_count = 0;
  {
    auto begin = Clock::now();
    const auto vertical = vertical_view.solve(true);
    const auto vertical_us = elapsed(begin);
    stats.vertical_us += vertical_us;
    recordPhase<CollectTiming>(stats.phase_timing.vertical_solving_us,
                               vertical_us);
    accumulate(stats.vertical_dyck, vertical.stats);
    stats.execution.peak_working_bytes = std::max(
        stats.execution.peak_working_bytes, vertical.stats.peak_working_bytes);
    begin = Clock::now();
    vertical_count = vertical.stats.components;
    std::vector<std::size_t> parent(vertical_count, kNone);
    for (const auto &edge : vertical.quotient_closing_edges) {
      if (parent[edge.source] == kNone)
        parent[edge.source] = edge.target;
      else if (parent[edge.source] != edge.target)
        throw std::logic_error(
            "vertical one-counter parent is not well-defined");
    }
    labels.reserve(object_count);
    for (std::size_t v = 0; v < graph.vertex_count; ++v)
      labels.push_back({{vertical.component[v * width], 0}, v});
    for (std::size_t v = 0; v < graph.vertex_count; ++v) {
      auto component = vertical.component[v * width + threshold];
      std::size_t residual_height = 0;
      for (std::size_t h = 0; h < width; ++h) {
        labels.push_back(
            {{component, residual_height}, graph.vertex_count + v * width + h});
        if (residual_height == 0 && parent[component] != kNone)
          component = parent[component];
        else
          ++residual_height;
      }
    }
    stats.execution.peak_working_bytes =
        std::max(stats.execution.peak_working_bytes,
                 (vertical.component.capacity() + parent.capacity()) *
                         sizeof(std::size_t) +
                     labels.capacity() * sizeof(LabeledObject) +
                     vertical.quotient_closing_edges.capacity() *
                         sizeof(LabeledStateEdge));
    const auto parent_map_labeling_us = elapsed(begin);
    stats.merge_us += parent_map_labeling_us;
    recordPhase<CollectTiming>(stats.phase_timing.parent_map_labeling_us,
                               parent_map_labeling_us);
  }
  auto begin = Clock::now();
  sortVerticalLabels(labels, width, vertical_count, stats);
  DisjointSets merged(object_count);
  stats.execution.peak_working_bytes = std::max(
      stats.execution.peak_working_bytes,
      merged.payloadBytes() + labels.capacity() * sizeof(LabeledObject));
  for (std::size_t i = 1; i < labels.size(); ++i)
    if (labels[i - 1].label == labels[i].label)
      merged.join(labels[i - 1].object, labels[i].object);
  std::vector<LabeledObject>().swap(labels);
  const auto vertical_unions_us = elapsed(begin);
  stats.merge_us += vertical_unions_us;
  recordPhase<CollectTiming>(stats.phase_timing.boundary_unions_us,
                             vertical_unions_us);

  {
    const auto horizontal_view = measurePhase<CollectTiming>(
        stats.phase_timing.horizontal_construction_us,
        [&] { return LiftedCounterGraph(graph, threshold, false); });
    stats.horizontal_arcs += horizontal_view.arcs;
    begin = Clock::now();
    const auto horizontal = horizontal_view.solve();
    const auto horizontal_us = elapsed(begin);
    stats.horizontal_us += horizontal_us;
    recordPhase<CollectTiming>(stats.phase_timing.horizontal_solving_us,
                               horizontal_us);
    accumulate(stats.horizontal_dyck, horizontal.stats);
    stats.execution.peak_working_bytes =
        std::max(stats.execution.peak_working_bytes,
                 merged.payloadBytes() + horizontal.stats.peak_working_bytes);
    begin = Clock::now();
    std::vector<std::size_t> representatives(horizontal.stats.components,
                                             kNone);
    stats.execution.peak_working_bytes =
        std::max(stats.execution.peak_working_bytes,
                 merged.payloadBytes() + (horizontal.component.capacity() +
                                          representatives.capacity()) *
                                             sizeof(std::size_t));
    for (std::size_t v = 0; v < graph.vertex_count; ++v)
      for (std::size_t h = 0; h < width; ++h) {
        const auto component = horizontal.component[v * width + h];
        const auto boundary = graph.vertex_count + v * width + h;
        auto &representative = representatives[component];
        if (representative == kNone)
          representative = boundary;
        else
          merged.join(representative, boundary);
      }
    const auto horizontal_unions_us = elapsed(begin);
    stats.merge_us += horizontal_unions_us;
    recordPhase<CollectTiming>(stats.phase_timing.boundary_unions_us,
                               horizontal_unions_us);
  }

  begin = Clock::now();
  std::vector<std::size_t> result(graph.vertex_count);
  std::unordered_map<std::size_t, std::size_t> identifiers;
  identifiers.reserve(graph.vertex_count);
  for (std::size_t v = 0; v < graph.vertex_count; ++v)
    result[v] =
        identifiers.emplace(merged.find(v), identifiers.size()).first->second;
  stats.execution.peak_working_bytes = std::max(
      stats.execution.peak_working_bytes,
      merged.payloadBytes() + result.capacity() * sizeof(std::size_t) +
          identifiers.size() * sizeof(decltype(identifiers)::value_type) +
          identifiers.bucket_count() * sizeof(void *));
  const auto merge_finalization_us = elapsed(begin);
  stats.merge_us += merge_finalization_us;
  recordPhase<CollectTiming>(stats.phase_timing.boundary_unions_us,
                             merge_finalization_us);
  return result;
}

struct PartitionData {
  std::unordered_map<Vertex, std::size_t> components;
  AdaptiveStats stats;
};

template <bool CollectTiming>
PartitionData
computePartition(const UnaryProjection &canonical, const UnaryGraph &processed,
                 const std::vector<std::size_t> &original_to_processed,
                 std::optional<std::size_t> threshold, bool sparsified) {
  PartitionData result;
  auto &stats = result.stats;
  stats.input_vertices = canonical.graph.vertex_count;
  stats.input_arcs = canonical.original_arc_count;
  stats.quotient_vertices = processed.vertex_count;
  stats.quotient_arcs = processed.edges.size();
  stats.added_reverse_arcs = canonical.added_reverse_arcs;
  stats.input_was_bidirected = canonical.added_reverse_arcs == 0;
  stats.overapproximates_original = canonical.added_reverse_arcs != 0;
  stats.sparsified = sparsified;
  // A shallow query retains its exact K even if all components take fast paths.
  if (threshold)
    stats.threshold = *threshold;
  auto begin = Clock::now();
  const auto parts = splitWeakComponents(processed);
  stats.execution.decomposition_us = elapsed(begin);
  recordPhase<CollectTiming>(stats.phase_timing.decomposition_us,
                             stats.execution.decomposition_us);
  stats.execution.weak_components = parts.size();
  std::vector<std::size_t> processed_components(processed.vertex_count);
  std::size_t offset = 0;
  begin = Clock::now();
  for (const auto &part : parts) {
    const auto n = part.graph.vertex_count;
    stats.execution.largest_component_vertices =
        std::max(stats.execution.largest_component_vertices, n);
    std::vector<std::size_t> local;
    if (n == 1) {
      local = {0};
      ++stats.execution.trivial_components;
    } else if (part.counter_mask != 3) {
      // The unused counter stays zero, so this is exact for every shallow K.
      auto single = singleCounter(part.graph);
      accumulate(stats.single_counter_dyck, single.stats);
      stats.execution.peak_working_bytes = std::max(
          stats.execution.peak_working_bytes, single.stats.peak_working_bytes);
      local = std::move(single.component);
      ++stats.execution.single_counter_components;
    } else {
      const auto bound =
          threshold ? *threshold : checkedMultiply(6, n, "adaptive threshold");
      local = shallowComponents<CollectTiming>(part.graph, bound, stats);
    }
    std::size_t count = 0;
    for (std::size_t v = 0; v < n; ++v) {
      processed_components[part.original_vertices[v]] = offset + local[v];
      count = std::max(count, local[v] + 1);
    }
    offset += count;
  }
  stats.execution.solving_us = elapsed(begin);
  begin = Clock::now();
  std::vector<std::size_t> identifiers(offset, kNone);
  std::size_t identifier_count = 0;
  result.components.reserve(canonical.vertices.size());
  for (std::size_t v = 0; v < canonical.vertices.size(); ++v) {
    const auto component = processed_components[original_to_processed[v]];
    if (identifiers[component] == kNone)
      identifiers[component] = identifier_count++;
    result.components.emplace(canonical.vertices[v], identifiers[component]);
  }
  stats.execution.lifting_us = elapsed(begin);
  recordPhase<CollectTiming>(stats.phase_timing.output_lifting_us,
                             stats.execution.lifting_us);
  return result;
}

template <bool CollectTiming>
PartitionData solveAdaptive(const Graph &graph,
                            const AdaptiveOptions &options) {
  const auto start = Clock::now();
  const auto canonical = projectToUnary(graph, options.input_policy);
  const auto projection_us = elapsed(start);
  PartitionData partition;
  std::uint64_t preprocessing_us = 0;
  if (options.sparsify) {
    const auto begin = Clock::now();
    const auto quotient = sparsifyUnaryGraph(canonical.graph);
    preprocessing_us = elapsed(begin);
    partition = computePartition<CollectTiming>(
        canonical, quotient.graph, quotient.original_to_quotient,
        std::nullopt, true);
    partition.stats.quotient_dyck = quotient.dyck;
    partition.stats.execution.peak_working_bytes =
        std::max(partition.stats.execution.peak_working_bytes,
                 quotient.dyck.peak_working_bytes);
  } else {
    std::vector<std::size_t> identity(canonical.graph.vertex_count);
    std::iota(identity.begin(), identity.end(), std::size_t(0));
    partition = computePartition<CollectTiming>(
        canonical, canonical.graph, identity, std::nullopt, false);
  }
  partition.stats.phase_timing.enabled = CollectTiming;
  recordPhase<CollectTiming>(partition.stats.phase_timing.projection_us,
                             projection_us);
  recordPhase<CollectTiming>(
      partition.stats.phase_timing.quotient_sparsification_us,
      preprocessing_us);
  partition.stats.execution.projection_us = projection_us;
  partition.stats.execution.preprocessing_us = preprocessing_us;
  partition.stats.execution.total_us = elapsed(start);
  return partition;
}

template <bool CollectTiming>
PartitionData solveShallowAdaptive(const Graph &graph, std::size_t threshold,
                                   const AdaptiveOptions &options) {
  const auto start = Clock::now();
  const auto canonical = projectToUnary(graph, options.input_policy);
  const auto projection_us = elapsed(start);
  std::vector<std::size_t> identity(canonical.graph.vertex_count);
  std::iota(identity.begin(), identity.end(), std::size_t(0));
  auto partition = computePartition<CollectTiming>(
      canonical, canonical.graph, identity, threshold, false);
  partition.stats.phase_timing.enabled = CollectTiming;
  recordPhase<CollectTiming>(partition.stats.phase_timing.projection_us,
                             projection_us);
  partition.stats.execution.projection_us = projection_us;
  partition.stats.execution.total_us = elapsed(start);
  return partition;
}

} // namespace

std::size_t AdaptiveResult::component(Vertex vertex) const {
  const auto found = components_.find(vertex);
  if (found == components_.end())
    throw std::out_of_range("unknown adaptive interleaved-Dyck vertex");
  return found->second;
}

bool AdaptiveResult::connected(Vertex first, Vertex second) const {
  return component(first) == component(second);
}

AdaptiveResult AdaptiveSolver::solve(
    const Graph &graph, const AdaptiveOptions &options) const {
  auto partition = options.collect_phase_timing
                       ? solveAdaptive<true>(graph, options)
                       : solveAdaptive<false>(graph, options);
  AdaptiveResult result;
  result.components_ = std::move(partition.components);
  result.stats_ = std::move(partition.stats);
  return result;
}

AdaptiveResult AdaptiveSolver::solveShallow(
    const Graph &graph, std::size_t threshold,
    const AdaptiveOptions &options) const {
  auto partition = options.collect_phase_timing
                       ? solveShallowAdaptive<true>(graph, threshold, options)
                       : solveShallowAdaptive<false>(graph, threshold, options);
  AdaptiveResult result;
  result.components_ = std::move(partition.components);
  result.stats_ = std::move(partition.stats);
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::unary
