#include "CFL/InterleavedDyck/SPDS/ValueFlow.h"

#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace lotus::cfl::interleaved_dyck::spds {
namespace {
using Adjacency = std::unordered_map<Vertex, std::vector<Vertex>>;
std::unordered_set<Vertex> reachable(const Adjacency &edges,
                                     std::vector<Vertex> pending) {
  std::unordered_set<Vertex> visited;
  while (!pending.empty()) {
    const auto v = pending.back();
    pending.pop_back();
    if (!visited.insert(v).second)
      continue;
    auto it = edges.find(v);
    if (it != edges.end())
      pending.insert(pending.end(), it->second.begin(), it->second.end());
  }
  return visited;
}
} // namespace

ValueFlowAnalysis::ValueFlowAnalysis(const Graph &graph, Options options) {
  if (options.parentheses != StackAcceptance::Empty ||
      options.brackets != StackAcceptance::Empty)
    throw std::invalid_argument("value-flow requires empty endpoint stacks");
  originals_ = graph.vertices();
  if (originals_.size() >
      static_cast<std::size_t>(std::numeric_limits<Vertex>::max() / 3))
    throw std::overflow_error("value-flow graph is too large");
  for (std::size_t i = 0; i < originals_.size(); ++i)
    ids_.emplace(originals_[i], static_cast<Vertex>(i) * 3);

  Adjacency forward, backward;
  std::vector<Vertex> starts, ends;
  for (const auto &e : graph.edges()) {
    forward[e.source].push_back(e.target);
    backward[e.target].push_back(e.source);
    if (e.label.kind == LabelKind::OpenBracket)
      starts.push_back(e.source);
    if (e.label.kind == LabelKind::CloseBracket)
      ends.push_back(e.target);
  }
  const auto after = reachable(forward, starts);
  const auto before = reachable(backward, ends);
  Graph product;
  std::set<Vertex> sources, targets;
  for (const auto &e : graph.edges()) {
    if (!after.count(e.source) || !after.count(e.target) ||
        !before.count(e.source) || !before.count(e.target))
      continue;
    const auto s = ids_.at(e.source), t = ids_.at(e.target);
    product.addEdge(s + 1, t + 1, e.label);
    if (e.label.kind == LabelKind::OpenBracket) {
      product.addEdge(s, t + 1, e.label);
      sources.insert(s);
    }
    if (e.label.kind == LabelKind::CloseBracket) {
      product.addEdge(s + 1, t + 2, e.label);
      targets.insert(t + 2);
    }
  }
  sources_.assign(sources.begin(), sources.end());
  targets_.assign(targets.begin(), targets.end());
  analysis_.emplace(Solver(options).prepare(product));
}

void ValueFlowAnalysis::validate(Vertex v) const {
  if (!ids_.count(v))
    throw std::invalid_argument("SPDS query vertex is not in graph");
}
Result ValueFlowAnalysis::restore(Result result) const {
  for (PairSet *relation : {&result.upper_bound, &result.parenthesis_pairs,
                            &result.bracket_pairs}) {
    PairSet mapped;
    for (const auto &p : *relation) {
      const Pair original{originals_.at(p.source / 3),
                          originals_.at(p.target / 3)};
      if (original.source != original.target)
        mapped.insert(original);
    }
    *relation = std::move(mapped);
  }
  return result;
}
Result ValueFlowAnalysis::analyzeAll(DemandDirection direction) const {
  return restore(analysis_->analyzeBetween(sources_, targets_, direction));
}
Result ValueFlowAnalysis::analyzeFrom(Vertex source) const {
  validate(source);
  const auto s = ids_.at(source);
  if (!std::binary_search(sources_.begin(), sources_.end(), s))
    return analysis_->analyzeBetween({}, targets_);
  return restore(
      analysis_->analyzeBetween({s}, targets_, DemandDirection::Post));
}
Result ValueFlowAnalysis::analyzeTo(Vertex target) const {
  validate(target);
  const auto t = ids_.at(target) + 2;
  if (!std::binary_search(targets_.begin(), targets_.end(), t))
    return analysis_->analyzeBetween(sources_, {});
  return restore(
      analysis_->analyzeBetween(sources_, {t}, DemandDirection::Pre));
}
Result ValueFlowAnalysis::analyzeDemands(const std::vector<Pair> &demands,
                                         DemandDirection direction) const {
  std::vector<Pair> mapped;
  for (const auto &p : demands) {
    validate(p.source);
    validate(p.target);
    const auto s = ids_.at(p.source), t = ids_.at(p.target) + 2;
    if (p.source != p.target &&
        std::binary_search(sources_.begin(), sources_.end(), s) &&
        std::binary_search(targets_.begin(), targets_.end(), t))
      mapped.push_back({s, t});
  }
  return restore(analysis_->analyzeDemands(mapped, direction));
}
} // namespace lotus::cfl::interleaved_dyck::spds
