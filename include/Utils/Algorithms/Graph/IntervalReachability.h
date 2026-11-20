#pragma once

#include "Utils/Algorithms/Graph/SCCGraph.h"

#include <memory>
#include <optional>
#include <utility>

namespace lotus {
namespace graph {

/// Half-open interval of target ranks, not vertex IDs.
struct RankInterval {
  Vertex begin, end;
  bool operator==(const RankInterval &other) const {
    return begin == other.begin && end == other.end;
  }
};

/// Exact, reflexive reachability in an immutable directed graph. Inspired by
/// PhASAR's IntervalReachabilityIndex; C++17 implementation using Lotus Tarjan
/// and bottom-up interval unions on the condensation DAG. Not a realizable-path
/// query for an ICFG: call/return matching must be encoded by the graph owner.
///
/// Build cost depends on the merged interval sets (worst-case quadratic space).
/// A query takes O(log K), where K is the source SCC's interval count.
/// Restricted construction indexes only vertices reachable from roots and only
/// selected target vertices. An unindexed source or unselected target returns
/// false.
class IntervalReachabilityIndex {
public:
  explicit IntervalReachabilityIndex(const Adjacency &graph)
      : IntervalReachabilityIndex(std::make_shared<SCCGraph>(graph)) {}
  explicit IntervalReachabilityIndex(std::shared_ptr<const SCCGraph> sccs)
      : m_sccs(std::move(sccs)) {
    if (!m_sccs)
      throw std::invalid_argument("Missing SCC graph");
    initialize(std::vector<bool>(m_sccs->size(), true), {});
  }
  IntervalReachabilityIndex(const Adjacency &graph,
                            const std::vector<Vertex> &roots,
                            const std::vector<bool> &targets = {})
      : m_sccs(std::make_shared<SCCGraph>(graph)) {
    std::vector<bool> indexed(m_sccs->size(), false);
    std::vector<Vertex> work;
    for (auto root : roots) {
      auto c = m_sccs->componentOf(root);
      if (!indexed[c]) {
        indexed[c] = true;
        work.push_back(c);
      }
    }
    while (!work.empty()) {
      auto c = work.back();
      work.pop_back();
      for (auto next : m_sccs->successors(c))
        if (!indexed[next]) {
          indexed[next] = true;
          work.push_back(next);
        }
    }
    initialize(std::move(indexed), targets);
  }
  size_t numVertices() const { return m_sccs->numVertices(); }
  size_t numTargets() const { return m_targets.size(); }
  size_t numSCCs() const { return m_sccs->size(); }
  bool isIndexed(Vertex v) const {
    return v < numVertices() && m_indexed[m_sccs->componentOf(v)];
  }
  bool isTarget(Vertex v) const {
    return v < numVertices() && m_rank[v].has_value();
  }
  std::optional<Vertex> rankOf(Vertex v) const {
    return v < numVertices() ? m_rank[v] : std::nullopt;
  }
  bool reaches(Vertex from, Vertex to) const {
    if (!isIndexed(from) || !isTarget(to))
      return false;
    const auto &ranges = intervalsFrom(from);
    auto rank = *m_rank[to];
    auto it = std::upper_bound(
        ranges.begin(), ranges.end(), rank,
        [](Vertex r, RankInterval interval) { return r < interval.begin; });
    return it != ranges.begin() && rank < std::prev(it)->end;
  }
  const std::vector<RankInterval> &intervalsFrom(Vertex from) const {
    static const std::vector<RankInterval> empty;
    return isIndexed(from) ? m_ranges[m_sccs->componentOf(from)] : empty;
  }
  template <typename Callback>
  void forEachReachableTarget(Vertex from, Callback callback) const {
    for (auto interval : intervalsFrom(from))
      for (auto rank = interval.begin; rank < interval.end; ++rank)
        callback(m_targets[rank]);
  }
  size_t numIntervals() const {
    size_t count = 0;
    for (const auto &row : m_ranges)
      count += row.size();
    return count;
  }
  std::shared_ptr<const SCCGraph> components() const { return m_sccs; }

private:
  void initialize(std::vector<bool> indexed, const std::vector<bool> &targets) {
    if (!targets.empty() && targets.size() != numVertices())
      throw std::invalid_argument("Target mask has incorrect size");
    m_indexed = std::move(indexed);
    m_rank.resize(numVertices());
    m_ranges.resize(numSCCs());
    for (Vertex c = 0; c < numSCCs(); ++c) {
      if (!m_indexed[c])
        continue;
      auto start = static_cast<Vertex>(m_targets.size());
      for (auto v : m_sccs->members(c))
        if (targets.empty() || targets[v]) {
          m_rank[v] = static_cast<Vertex>(m_targets.size());
          m_targets.push_back(v);
        }
      auto &ranges = m_ranges[c];
      if (start != m_targets.size())
        ranges.push_back({start, static_cast<Vertex>(m_targets.size())});
      for (auto next : m_sccs->successors(c)) {
        const auto &child = m_ranges[next];
        ranges.insert(ranges.end(), child.begin(), child.end());
      }
      std::sort(
          ranges.begin(), ranges.end(),
          [](RankInterval a, RankInterval b) { return a.begin < b.begin; });
      size_t output = 0;
      for (auto interval : ranges) {
        if (output && interval.begin <= ranges[output - 1].end)
          ranges[output - 1].end =
              std::max(ranges[output - 1].end, interval.end);
        else
          ranges[output++] = interval;
      }
      ranges.resize(output);
    }
  }
  std::shared_ptr<const SCCGraph> m_sccs;
  std::vector<bool> m_indexed;
  std::vector<std::optional<Vertex>> m_rank;
  std::vector<Vertex> m_targets;
  std::vector<std::vector<RankInterval>> m_ranges;
};
} // namespace graph
} // namespace lotus
