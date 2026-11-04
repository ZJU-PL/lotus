#include "CFL/InterleavedDyck/SPDS/Solver.h"

#include <chrono>

namespace lotus::cfl::interleaved_dyck::spds {
namespace {

// 0 is the protected bottom; uint64_t keeps UINT_MAX+1 representable.
Symbol symbol(unsigned id) { return static_cast<Symbol>(id) + 1; }
std::vector<Symbol> stackWord(const std::vector<unsigned> &stack) {
  std::vector<Symbol> result;
  result.reserve(stack.size() + 1);
  for (unsigned id : stack)
    result.push_back(symbol(id));
  result.push_back(0);
  return result;
}
struct Projections {
  std::map<Vertex, State> controls;
  PushdownSystem<BooleanSemiring> calls, fields;
};
Projections project(const Graph &graph) {
  Projections result;
  auto vertices = graph.vertices();
  std::sort(vertices.begin(), vertices.end());
  for (Vertex v : vertices) {
    State p = result.calls.addControl();
    result.fields.addControl();
    result.controls.emplace(v, p);
  }
  auto build = [&](PushdownSystem<BooleanSemiring> &system, bool call) {
    for (const auto &e : graph.edges()) {
      const State from = result.controls.at(e.source),
                  to = result.controls.at(e.target);
      const auto open =
          call ? LabelKind::OpenParenthesis : LabelKind::OpenBracket;
      const auto close =
          call ? LabelKind::CloseParenthesis : LabelKind::CloseBracket;
      if (e.label.kind == close)
        system.addRule(from, symbol(e.label.id), to, {});
      else if (e.label.kind == open)
        system.addPushRule(from, to, symbol(e.label.id));
      else if (e.label.kind == LabelKind::OpenParenthesis ||
               e.label.kind == LabelKind::CloseParenthesis ||
               e.label.kind == LabelKind::OpenBracket ||
               e.label.kind == LabelKind::CloseBracket ||
               e.label.kind == LabelKind::Neutral)
        system.addPreserveRule(from, to);
      else
        throw std::invalid_argument("invalid SPDS graph label kind");
    }
  };
  build(result.calls, true);
  build(result.fields, false);
  return result;
}
Automaton<BooleanSemiring>
saturate(const PushdownSystem<BooleanSemiring> &system, State anchor,
         Direction direction, Limits limits) {
  auto seed = RegularSet::singleton(system.controls(), {anchor, {0}});
  return direction == Direction::Post ? postStar(system, seed, limits)
                                      : preStar(system, seed, limits);
}
void accumulate(Statistics &to, const Statistics &from) {
  to.states += from.states;
  to.transitions += from.transitions;
  to.updates += from.updates;
  to.processed += from.processed;
  to.rules += from.rules;
  to.setup_microseconds += from.setup_microseconds;
  to.saturation_microseconds += from.saturation_microseconds;
  to.readout_microseconds += from.readout_microseconds;
  to.projection_microseconds += from.projection_microseconds;
}
} // namespace

QueryResult::QueryResult(
    Vertex anchor, Direction direction, Options options,
    std::shared_ptr<const std::map<Vertex, State>> controls,
    Automaton<BooleanSemiring> calls, Automaton<BooleanSemiring> fields)
    : anchor_(anchor), direction_(direction), options_(options),
      controls_(std::move(controls)), calls_(std::move(calls)),
      fields_(std::move(fields)) {}
bool QueryResult::reachable(const Automaton<BooleanSemiring> &a, Vertex v,
                            StackAcceptance acceptance) const {
  auto p = controls_->find(v);
  if (p == controls_->end())
    return false;
  return acceptance == StackAcceptance::Empty ? a.accepts(p->second, {0})
                                              : a.acceptsPrefix(p->second, {});
}
bool QueryResult::parenthesisReachable(Vertex v) const {
  return reachable(calls_, v, options_.parentheses);
}
bool QueryResult::bracketReachable(Vertex v) const {
  return reachable(fields_, v, options_.brackets);
}
bool QueryResult::mayReach(Vertex v) const {
  return parenthesisReachable(v) && bracketReachable(v);
}
bool QueryResult::mayAccept(Vertex v, const std::vector<unsigned> &parentheses,
                            const std::vector<unsigned> &brackets) const {
  auto p = controls_->find(v);
  return p != controls_->end() &&
         calls_.accepts(p->second, stackWord(parentheses)) &&
         fields_.accepts(p->second, stackWord(brackets));
}
PreparedAnalysis::PreparedAnalysis(Options options, const Graph &graph,
                                   std::map<Vertex, State> controls,
                                   PushdownSystem<BooleanSemiring> calls,
                                   PushdownSystem<BooleanSemiring> fields)
    : options_(options),
      controls_(
          std::make_shared<const std::map<Vertex, State>>(std::move(controls))),
      calls_(std::move(calls)), fields_(std::move(fields)),
      vertices_(controls_->size()), edges_(graph.edges()),
      successors_(controls_->size()), predecessors_(controls_->size()) {
  for (const auto &entry : *controls_)
    vertices_[entry.second] = entry.first;
  for (const auto &edge : edges_) {
    const State from = controls_->at(edge.source);
    const State to = controls_->at(edge.target);
    successors_[from].push_back(to);
    predecessors_[to].push_back(from);
  }
}
std::optional<Graph>
PreparedAnalysis::relevantGraph(Vertex anchor, Direction direction) const {
  const State start = controls_->at(anchor);
  const auto &adjacency =
      direction == Direction::Post ? successors_ : predecessors_;
  std::vector<bool> live(controls_->size(), false);
  std::vector<State> worklist{start};
  live[start] = true;
  for (std::size_t cursor = 0; cursor < worklist.size(); ++cursor)
    for (State next : adjacency[worklist[cursor]])
      if (!live[next]) {
        live[next] = true;
        worklist.push_back(next);
      }
  if (worklist.size() == controls_->size())
    return std::nullopt;
  Graph result;
  for (State state : worklist)
    result.addVertex(vertices_[state]);
  for (const auto &edge : edges_)
    if (live[controls_->at(edge.source)] && live[controls_->at(edge.target)])
      result.addEdge(edge.source, edge.target, edge.label);
  return result;
}
QueryResult PreparedAnalysis::query(Vertex anchor, Direction direction,
                                    bool slice_graph) const {
  auto found = controls_->find(anchor);
  if (found == controls_->end())
    throw std::invalid_argument("SPDS query vertex is not in graph");
  if (slice_graph) {
    const auto slice_started = std::chrono::steady_clock::now();
    if (auto graph = relevantGraph(anchor, direction)) {
      auto systems = project(*graph);
      const auto slice_projection_microseconds = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - slice_started)
              .count());
      const State state = systems.controls.at(anchor);
      auto calls = saturate(systems.calls, state, direction, options_.limits);
      auto fields = saturate(systems.fields, state, direction, options_.limits);
      auto controls = std::make_shared<const std::map<Vertex, State>>(
          std::move(systems.controls));
      QueryResult result(anchor, direction, options_, std::move(controls),
                         std::move(calls), std::move(fields));
      result.projection_microseconds_ = slice_projection_microseconds;
      return result;
    }
  }
  auto calls = saturate(calls_, found->second, direction, options_.limits);
  auto fields = saturate(fields_, found->second, direction, options_.limits);
  return QueryResult(anchor, direction, options_, controls_, std::move(calls),
                     std::move(fields));
}
QueryResult PreparedAnalysis::queryFrom(Vertex source) const {
  return query(source, Direction::Post);
}
QueryResult PreparedAnalysis::queryTo(Vertex target) const {
  return query(target, Direction::Pre);
}
namespace {
std::vector<BooleanSemiring::Weight>
endpointWeights(const Automaton<BooleanSemiring> &automaton,
                StackAcceptance acceptance) {
  return acceptance == StackAcceptance::Empty
             ? automaton.controlWeights({0})
             : automaton.controlWeightsWithPrefix({});
}
} // namespace
Result PreparedAnalysis::analyzeFrom(Vertex source) const {
  auto query = queryFrom(source);
  Result result;
  result.statistics.projection_microseconds = projection_microseconds_;
  const auto calls =
      endpointWeights(query.callAutomaton(), options_.parentheses);
  const auto fields =
      endpointWeights(query.fieldAutomaton(), options_.brackets);
  result.statistics.projection_microseconds += query.projection_microseconds_;
  accumulate(result.statistics, query.callAutomaton().statistics());
  accumulate(result.statistics, query.fieldAutomaton().statistics());
  for (const auto &target : *controls_) {
    const auto found = query.controls_->find(target.first);
    if (found == query.controls_->end())
      continue;
    const Pair pair{source, target.first};
    if (calls[found->second])
      result.parenthesis_pairs.insert(pair);
    if (fields[found->second])
      result.bracket_pairs.insert(pair);
    if (calls[found->second] && fields[found->second])
      result.upper_bound.insert(pair);
  }
  return result;
}
Result PreparedAnalysis::analyzeTo(Vertex target) const {
  auto query = queryTo(target);
  Result result;
  result.statistics.projection_microseconds = projection_microseconds_;
  const auto calls =
      endpointWeights(query.callAutomaton(), options_.parentheses);
  const auto fields =
      endpointWeights(query.fieldAutomaton(), options_.brackets);
  result.statistics.projection_microseconds += query.projection_microseconds_;
  accumulate(result.statistics, query.callAutomaton().statistics());
  accumulate(result.statistics, query.fieldAutomaton().statistics());
  for (const auto &source : *controls_) {
    const auto found = query.controls_->find(source.first);
    if (found == query.controls_->end())
      continue;
    const Pair pair{source.first, target};
    if (calls[found->second])
      result.parenthesis_pairs.insert(pair);
    if (fields[found->second])
      result.bracket_pairs.insert(pair);
    if (calls[found->second] && fields[found->second])
      result.upper_bound.insert(pair);
  }
  return result;
}
Result PreparedAnalysis::analyzeAll() const {
  Result result;
  result.statistics.projection_microseconds = projection_microseconds_;
  for (const auto &source : *controls_) {
    auto query = this->query(source.first, Direction::Post, false);
    const auto call_weights =
        endpointWeights(query.callAutomaton(), options_.parentheses);
    const auto field_weights =
        endpointWeights(query.fieldAutomaton(), options_.brackets);
    accumulate(result.statistics, query.callAutomaton().statistics());
    accumulate(result.statistics, query.fieldAutomaton().statistics());
    for (const auto &target : *controls_) {
      const Pair pair{source.first, target.first};
      const bool call = call_weights[target.second];
      const bool field = field_weights[target.second];
      if (call)
        result.parenthesis_pairs.insert(pair);
      if (field)
        result.bracket_pairs.insert(pair);
      if (call && field)
        result.upper_bound.insert(pair);
    }
  }
  return result;
}
Result PreparedAnalysis::analyzeBetween(const std::vector<Vertex> &sources,
                                        const std::vector<Vertex> &targets,
                                        DemandDirection direction) const {
  std::set<Vertex> from(sources.begin(), sources.end());
  std::set<Vertex> to(targets.begin(), targets.end());
  for (const auto &endpoints : {&from, &to})
    for (Vertex v : *endpoints)
      if (!controls_->count(v))
        throw std::invalid_argument("SPDS query vertex is not in graph");
  const bool post =
      direction == DemandDirection::Post ||
      (direction == DemandDirection::Auto && from.size() <= to.size());
  Result result;
  result.statistics.projection_microseconds = projection_microseconds_;
  if (from.empty() || to.empty())
    return result;
  for (Vertex anchor : post ? from : to) {
    auto q = post ? queryFrom(anchor) : queryTo(anchor);
    const auto calls = endpointWeights(q.callAutomaton(), options_.parentheses);
    const auto fields = endpointWeights(q.fieldAutomaton(), options_.brackets);
    accumulate(result.statistics, q.callAutomaton().statistics());
    accumulate(result.statistics, q.fieldAutomaton().statistics());
    result.statistics.projection_microseconds += q.projection_microseconds_;
    for (Vertex endpoint : post ? to : from) {
      const auto found = q.controls_->find(endpoint);
      if (found == q.controls_->end())
        continue;
      const Pair pair = post ? Pair{anchor, endpoint} : Pair{endpoint, anchor};
      const State state = found->second;
      if (calls[state])
        result.parenthesis_pairs.insert(pair);
      if (fields[state])
        result.bracket_pairs.insert(pair);
      if (calls[state] && fields[state])
        result.upper_bound.insert(pair);
    }
  }
  return result;
}

Result PreparedAnalysis::analyzeDemands(const std::vector<Pair> &demands,
                                        DemandDirection direction) const {
  std::map<Vertex, std::vector<Vertex>> by_source, by_target;
  std::set<Pair, bool (*)(const Pair &, const Pair &)> unique(
      [](const Pair &a, const Pair &b) {
        return std::tie(a.source, a.target) < std::tie(b.source, b.target);
      });
  for (const Pair &pair : demands) {
    if (!controls_->count(pair.source) || !controls_->count(pair.target))
      throw std::invalid_argument("SPDS demand vertex is not in graph");
    if (unique.insert(pair).second) {
      by_source[pair.source].push_back(pair.target);
      by_target[pair.target].push_back(pair.source);
    }
  }
  const bool use_post = direction == DemandDirection::Post ||
                        (direction == DemandDirection::Auto &&
                         by_source.size() <= by_target.size());
  Result result;
  result.statistics.projection_microseconds = projection_microseconds_;
  if (use_post) {
    for (const auto &entry : by_source) {
      auto query = queryFrom(entry.first);
      const auto calls =
          endpointWeights(query.callAutomaton(), options_.parentheses);
      const auto fields =
          endpointWeights(query.fieldAutomaton(), options_.brackets);
      result.statistics.projection_microseconds +=
          query.projection_microseconds_;
      accumulate(result.statistics, query.callAutomaton().statistics());
      accumulate(result.statistics, query.fieldAutomaton().statistics());
      for (Vertex target : entry.second) {
        const Pair pair{entry.first, target};
        const auto found = query.controls_->find(target);
        if (found == query.controls_->end())
          continue;
        const State state = found->second;
        if (calls[state])
          result.parenthesis_pairs.insert(pair);
        if (fields[state])
          result.bracket_pairs.insert(pair);
        if (calls[state] && fields[state])
          result.upper_bound.insert(pair);
      }
    }
  } else {
    for (const auto &entry : by_target) {
      auto query = queryTo(entry.first);
      const auto calls =
          endpointWeights(query.callAutomaton(), options_.parentheses);
      const auto fields =
          endpointWeights(query.fieldAutomaton(), options_.brackets);
      result.statistics.projection_microseconds +=
          query.projection_microseconds_;
      accumulate(result.statistics, query.callAutomaton().statistics());
      accumulate(result.statistics, query.fieldAutomaton().statistics());
      for (Vertex source : entry.second) {
        const Pair pair{source, entry.first};
        const auto found = query.controls_->find(source);
        if (found == query.controls_->end())
          continue;
        const State state = found->second;
        if (calls[state])
          result.parenthesis_pairs.insert(pair);
        if (fields[state])
          result.bracket_pairs.insert(pair);
        if (calls[state] && fields[state])
          result.upper_bound.insert(pair);
      }
    }
  }
  return result;
}
PreparedAnalysis Solver::prepare(const Graph &graph) const {
  const auto started = std::chrono::steady_clock::now();
  auto systems = project(graph);
  PreparedAnalysis result(options_, graph, std::move(systems.controls),
                          std::move(systems.calls), std::move(systems.fields));
  result.projection_microseconds_ = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started)
          .count());
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::spds
