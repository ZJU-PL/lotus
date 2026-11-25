#include "CFL/InterleavedDyck/StagedBounds/Algorithms.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::staged_bounds::detail {

std::vector<Graph> weakComponents(const Graph &graph) {
  std::unordered_map<Vertex, std::vector<Vertex>> undirected;
  for (const Edge &edge : graph.edges()) {
    undirected[edge.source].push_back(edge.target);
    undirected[edge.target].push_back(edge.source);
  }

  std::unordered_map<Vertex, std::size_t> component_of;
  std::size_t component_count = 0;
  for (Vertex vertex : graph.vertices()) {
    if (component_of.count(vertex) != 0U) {
      continue;
    }
    std::deque<Vertex> worklist{vertex};
    component_of[vertex] = component_count;
    while (!worklist.empty()) {
      const Vertex current = worklist.front();
      worklist.pop_front();
      for (Vertex next : undirected[current]) {
        if (component_of.emplace(next, component_count).second) {
          worklist.push_back(next);
        }
      }
    }
    ++component_count;
  }

  std::vector<Graph> components(component_count);
  for (const Edge &edge : graph.edges()) {
    components[component_of.at(edge.source)].addEdge(edge.source, edge.target,
                                                     edge.label);
  }
  return components;
}

PairSet mutualRefinementImpl(const Graph &input, GrammarStrength strength,
                             unsigned parity_groups, BenchmarkKind benchmark,
                             bool factorized_tracing,
                             MutualRefinementMode mode,
                             const std::optional<Pair> &target = std::nullopt) {
  const std::vector<Graph> components = weakComponents(input);
  if (components.size() > 1U) {
    PairSet result;
    for (const Graph &component : components) {
      PairSet component_result =
          mutualRefinementImpl(component, strength, parity_groups, benchmark,
                               factorized_tracing, mode, target);
      result.insert(component_result.begin(), component_result.end());
    }
    return result;
  }

  Graph graph =
      target ? removeNotOnCandidatePaths(input, PairSet{*target}) : input;
  graph = retainMatchedLabels(graph);
  PairSet alpha_paths;
  PairSet beta_paths;

  while (!graph.empty()) {
    const std::size_t old_edge_count = graph.edges().size();
    auto alpha = runProjected(graph, Alphabet::Parenthesis, strength,
                              parity_groups, true, factorized_tracing, target,
                              mode);
    alpha_paths = std::move(alpha.pairs);
    if (target && alpha_paths.count(*target) == 0U) {
      return {};
    }
    graph = retainMatchedLabels(alpha.used_edges);
    if (graph.empty()) {
      return {};
    }

    auto beta = runProjected(graph, Alphabet::Bracket, strength, parity_groups,
                             true, factorized_tracing, target, mode);
    beta_paths = std::move(beta.pairs);
    if (target && beta_paths.count(*target) == 0U) {
      return {};
    }
    graph = beta.used_edges;

    if (benchmark == BenchmarkKind::ValueFlow) {
      beta_paths = filterBracketPaths(graph, beta_paths);
      if (target && beta_paths.count(*target) == 0U) {
        return {};
      }
      graph = removeValueFlowUnreachable(graph);
    }
    graph = retainMatchedLabels(graph);

    if (graph.empty() || graph.edges().size() == old_edge_count) {
      break;
    }
  }

  PairSet result = intersect(alpha_paths, beta_paths);
  if (target) {
    if (result.count(*target) != 0U) {
      return PairSet{*target};
    }
    return {};
  }
  return result;
}

class DisjointSet {
public:
  explicit DisjointSet(const std::vector<Vertex> &vertices) {
    for (Vertex vertex : vertices) {
      parent_[vertex] = vertex;
      size_[vertex] = 1;
    }
  }

  Vertex find(Vertex vertex) {
    Vertex &parent = parent_.at(vertex);
    if (parent != vertex) {
      parent = find(parent);
    }
    return parent;
  }

  void join(Vertex left, Vertex right) {
    left = find(left);
    right = find(right);
    if (left == right) {
      return;
    }
    if (size_.at(left) < size_.at(right)) {
      std::swap(left, right);
    }
    parent_[right] = left;
    size_[left] += size_[right];
  }

private:
  std::unordered_map<Vertex, Vertex> parent_;
  std::unordered_map<Vertex, std::size_t> size_;
};

struct Condensation {
  Graph graph;
  std::unordered_map<Vertex, Vertex> root;
  std::unordered_map<Vertex, std::vector<Vertex>> groups;
};

Condensation condense(const Graph &graph, const PairSet &underapproximation) {
  DisjointSet sets(graph.vertices());
  for (const Pair &pair : underapproximation) {
    if (pair.source != pair.target &&
        underapproximation.count({pair.target, pair.source}) != 0U) {
      sets.join(pair.source, pair.target);
    }
  }

  Condensation result;
  for (Vertex vertex : graph.vertices()) {
    const Vertex root = sets.find(vertex);
    result.root[vertex] = root;
    result.groups[root].push_back(vertex);
  }
  for (const Edge &edge : graph.edges()) {
    result.graph.addEdge(result.root.at(edge.source),
                         result.root.at(edge.target), edge.label);
  }
  return result;
}

PairSet expandCondensedPairs(const Condensation &condensation,
                             const PairSet &condensed_pairs) {
  PairSet result;
  for (const Pair &pair : condensed_pairs) {
    const auto source_group = condensation.groups.find(pair.source);
    const auto target_group = condensation.groups.find(pair.target);
    if (source_group == condensation.groups.end() ||
        target_group == condensation.groups.end()) {
      continue;
    }
    for (Vertex source : source_group->second) {
      for (Vertex target : target_group->second) {
        if (source != target) {
          result.insert({source, target});
        }
      }
    }
  }
  for (const auto &[unused, group] : condensation.groups) {
    (void)unused;
    for (Vertex source : group) {
      for (Vertex target : group) {
        if (source != target) {
          result.insert({source, target});
        }
      }
    }
  }
  return result;
}

PairSet refinedWithCondensation(const Graph &graph,
                                const PairSet &underapproximation,
                                GrammarStrength strength,
                                unsigned parity_groups, BenchmarkKind benchmark,
                                bool factorized_tracing,
                                MutualRefinementMode mode) {
  const Condensation condensation = condense(graph, underapproximation);
  const PairSet condensed =
      mutualRefinementImpl(condensation.graph, strength, parity_groups,
                           benchmark, factorized_tracing, mode);
  return expandCondensedPairs(condensation, condensed);
}

PairSet onDemand(const Graph &graph, const PairSet &underapproximation,
                 const PairSet &overapproximation, GrammarStrength strength,
                 unsigned parity_groups, BenchmarkKind benchmark,
                 bool factorized_tracing, MutualRefinementMode mode) {
  const Condensation condensation = condense(graph, underapproximation);
  PairSet result = underapproximation;
  std::unordered_map<Pair, bool, PairHash> memory;
  for (const Pair &candidate : overapproximation) {
    if (underapproximation.count(candidate) != 0U) {
      continue;
    }
    const auto source = condensation.root.find(candidate.source);
    const auto target = condensation.root.find(candidate.target);
    if (source == condensation.root.end() ||
        target == condensation.root.end()) {
      continue;
    }
    const Pair root_pair{source->second, target->second};
    auto found = memory.find(root_pair);
    if (found == memory.end()) {
      const bool accepted =
          mutualRefinementImpl(condensation.graph, strength, parity_groups,
                               benchmark, factorized_tracing, mode, root_pair)
              .count(root_pair) != 0U;
      found = memory.emplace(root_pair, accepted).first;
    }
    if (found->second) {
      result.insert(candidate);
    }
  }
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds::detail

namespace lotus::cfl::interleaved_dyck::staged_bounds {

PairSet Solver::mutualRefinement(const Graph &graph, GrammarStrength strength,
                                 unsigned parity_groups,
                                 BenchmarkKind benchmark,
                                 bool factorized_tracing,
                                 MutualRefinementMode mode) const {
  return detail::mutualRefinementImpl(graph, strength, parity_groups, benchmark,
                                      factorized_tracing, mode);
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds
