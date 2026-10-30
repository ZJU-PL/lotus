#include "Verification/Automata/Detail/Summary.h"

#include <algorithm>

namespace lotus::verification::automata::detail {
std::uint64_t addCost(std::uint64_t a, std::uint64_t b) {
  if (a >= Infinity - b)
    throw ResourceLimit();
  return a + b;
}
SummaryGraph::SummaryGraph(std::shared_ptr<NestedWordAutomaton> a,
                           SearchOptions searchOptions, Context c)
    : automaton(std::move(a)), context(std::move(c)),
      workspace(context->maxWorkspaceBytes), paths(workspace.resource()),
      summaries(workspace.resource()), outgoing(workspace.resource()),
      incoming(workspace.resource()), options(std::move(searchOptions)),
      queue_(std::greater<QueueItem>{},
             std::pmr::vector<QueueItem>(workspace.resource())),
      index_(workspace.resource()), callsInto_(workspace.resource()) {
  outgoing.resize(automaton->stateCount());
  incoming.resize(automaton->stateCount());
  callsInto_.resize(automaton->stateCount());
  paths.reserve(automaton->stateCount() + automaton->transitions().size());
  for (const auto &t : automaton->transitions()) {
    context->check();
    if (automaton->alphabet().kind(t.letter) == LetterKind::Call)
      callsInto_[t.target].push_back(t);
  }
}
PathId SummaryGraph::epsilon(State q) {
  context->check();
  if (paths.size() >= context->maxWitnessNodes)
    throw ResourceLimit();
  paths.push_back({q, q, 0, automaton->isFinal(q), false});
  return paths.size() - 1;
}
std::uint64_t SummaryGraph::edgeCost(const Transition &t) const {
  std::uint64_t cost = 1;
  if (options.strategy == SearchStrategy::LoopPenalizingAStar) {
    if (automaton->alphabet().kind(t.letter) == LetterKind::Call)
      cost = addCost(cost, options.callPenalty);
    if (std::find(options.loopHeads.begin(), options.loopHeads.end(),
                  t.target) != options.loopHeads.end())
      cost = addCost(cost, options.loopPenalty);
  }
  if (options.extraCost)
    cost = addCost(cost, options.extraCost(t));
  return cost;
}
PathId SummaryGraph::edge(const Transition &t) {
  context->check();
  if (paths.size() >= context->maxWitnessNodes)
    throw ResourceLimit();
  paths.push_back({t.source, t.target, edgeCost(t),
                   automaton->isFinal(t.source) || automaton->isFinal(t.target),
                   true, NoPath, NoPath, t.letter, 1});
  return paths.size() - 1;
}
PathId SummaryGraph::concat(PathId left, PathId right) {
  context->check();
  const auto l = paths.at(left), r = paths.at(right);
  if (l.target != r.source)
    throw std::logic_error("disconnected NWA witness");
  if (!l.nonempty)
    return right;
  if (!r.nonempty)
    return left;
  if (l.length > std::numeric_limits<std::size_t>::max() - r.length)
    throw ResourceLimit();
  if (paths.size() >= context->maxWitnessNodes)
    throw ResourceLimit();
  paths.push_back({l.source, r.target, addCost(l.cost, r.cost),
                   l.accepting || r.accepting, true, left, right, Bottom,
                   l.length + r.length});
  return paths.size() - 1;
}
NestedRun SummaryGraph::expand(PathId id) const {
  NestedRun result;
  if (paths.at(id).length > context->maxPairs)
    throw ResourceLimit();
  result.states.reserve(paths[id].length + 1);
  result.word.reserve(paths[id].length);
  result.states.push_back(paths.at(id).source);
  std::vector<PathId> work;
  work.reserve(paths.size());
  work.push_back(id);
  {
    HotLoopScope hot(*context, "summary-witness-expansion");
    while (!work.empty()) {
      context->check();
      auto p = paths.at(work.back());
      work.pop_back();
      if (p.left != NoPath) {
        work.push_back(p.right);
        work.push_back(p.left);
      } else if (p.nonempty) {
        if (result.word.size() >= context->maxPairs)
          throw ResourceLimit();
        result.word.push_back(p.letter);
        result.states.push_back(p.target);
      }
    }
  }
  result.computeMatching(automaton->alphabet());
  return result;
}
void SummaryGraph::offer(PathId id) {
  context->check();
  const auto path = paths.at(id);
  unsigned flags = (path.accepting ? 1U : 0U) | (path.nonempty ? 2U : 0U);
  auto key = (std::uint64_t(path.source) << 32) | path.target;
  auto found = index_.find(key);
  if (found == index_.end())
    found = index_
                .emplace(key, std::array<std::size_t, 4>{NoPath, NoPath, NoPath,
                                                         NoPath})
                .first;
  auto &slot = found->second[flags];
  if (slot == NoPath) {
    if (summaries.size() >= context->maxPairs)
      throw ResourceLimit();
    slot = summaries.size();
    summaries.push_back({path.source, path.target, flags, id, false});
    queue_.push({path.cost, slot});
  } else if (!summaries[slot].settled &&
             path.cost < paths[summaries[slot].path].cost) {
    summaries[slot].path = id;
    queue_.push({path.cost, slot});
  }
}
bool SummaryGraph::improves(State source, State target, bool accepting,
                            bool nonempty, std::uint64_t cost) const {
  auto key = (std::uint64_t(source) << 32) | target;
  auto found = index_.find(key);
  if (found == index_.end())
    return true;
  auto slot = found->second[(accepting ? 1U : 0U) | (nonempty ? 2U : 0U)];
  return slot == NoPath ||
         (!summaries[slot].settled && cost < paths[summaries[slot].path].cost);
}
void SummaryGraph::offerConcat(PathId left, PathId right) {
  context->check();
  const auto l = paths[left], r = paths[right];
  if (improves(l.source, r.target, l.accepting || r.accepting,
               l.nonempty || r.nonempty, addCost(l.cost, r.cost)))
    offer(concat(left, right));
}
void SummaryGraph::compute() {
  HotLoopScope hot(*context, "balanced-summary-saturation");
  for (State q = 0; q < automaton->stateCount(); ++q)
    offer(epsilon(q));
  for (const auto &t : automaton->transitions()) {
    context->check();
    if (automaton->alphabet().kind(t.letter) == LetterKind::Internal)
      offer(edge(t));
  }
  while (!queue_.empty()) {
    context->check();
    const auto [cost, id] = queue_.top();
    queue_.pop();
    auto entry = summaries[id];
    if (entry.settled || paths[entry.path].cost != cost)
      continue;
    summaries[id].settled = true;
    outgoing[entry.source].push_back(id);
    incoming[entry.target].push_back(id);
    for (auto prior : incoming[entry.source]) {
      context->check();
      offerConcat(summaries[prior].path, entry.path);
    }
    for (auto next : outgoing[entry.target]) {
      context->check();
      offerConcat(entry.path, summaries[next].path);
    }
    // A balanced summary may be wrapped in a matching call and return.
    for (const auto &call : callsInto_[entry.source]) {
      context->check();
      for (Letter r : automaton->enabledLetters(
               entry.target, LetterKind::Return, call.source)) {
        context->check();
        for (State target :
             automaton->successors(entry.target, r, call.source)) {
          context->check();
          Transition ret{entry.target, r, target, call.source};
          auto cost = addCost(addCost(edgeCost(call), paths[entry.path].cost),
                              edgeCost(ret));
          bool accepting = paths[entry.path].accepting ||
                           automaton->isFinal(call.source) ||
                           automaton->isFinal(target);
          if (!improves(call.source, target, accepting, true, cost))
            continue;
          PathId left = edge(call);
          PathId right = edge(ret);
          offer(concat(concat(left, entry.path), right));
        }
      }
    }
  }
}

Prefixes prefixes(SummaryGraph &g, bool stopAtFinal) {
  Prefixes result;
  auto n = g.automaton->stateCount();
  result.paths.assign(2 * n, NoPath);
  using Item = std::pair<std::uint64_t, std::pair<std::uint64_t, std::size_t>>;
  std::priority_queue<Item, std::pmr::vector<Item>, std::greater<Item>> queue(
      std::greater<Item>{}, std::pmr::vector<Item>(g.workspace.resource()));
  std::uint64_t ticket = 0;
  auto offer = [&](std::size_t vertex, PathId path) {
    g.context->check();
    auto cost = g.paths[path].cost;
    if (result.paths[vertex] == NoPath ||
        cost < g.paths[result.paths[vertex]].cost) {
      result.paths[vertex] = path;
      auto heuristic =
          (stopAtFinal &&
           g.options.strategy == SearchStrategy::LoopPenalizingAStar &&
           g.options.heuristic)
              ? g.options.heuristic(static_cast<State>(vertex / 2))
              : 0;
      auto priority = g.options.strategy == SearchStrategy::SummaryReachability
                          ? ticket++
                          : addCost(cost, heuristic);
      queue.push({priority, {cost, vertex}});
    }
  };
  auto extend = [&](std::size_t vertex, PathId left, PathId right) {
    g.context->check();
    auto cost = addCost(g.paths[left].cost, g.paths[right].cost);
    if (result.paths[vertex] == NoPath ||
        cost < g.paths[result.paths[vertex]].cost)
      offer(vertex, g.concat(left, right));
  };
  HotLoopScope hot(*g.context, "summary-prefix-search");
  for (State q : g.automaton->initialStates())
    offer(2 * std::size_t(q), g.epsilon(q));
  while (!queue.empty()) {
    g.context->check();
    const auto item = queue.top().second;
    queue.pop();
    auto v = item.second;
    auto path = result.paths[v];
    if (g.paths[path].cost != item.first)
      continue;
    State q = static_cast<State>(v / 2);
    bool aboveBottom = v % 2;
    if (stopAtFinal && g.automaton->isFinal(q)) {
      result.goal = v;
      return result;
    }
    for (auto id : g.outgoing[q]) {
      g.context->check();
      const auto s = g.summaries[id];
      extend(2 * std::size_t(s.target) + aboveBottom, path, s.path);
    }
    if (g.options.semantics == WordSemantics::Closed)
      continue;
    for (Letter a : g.automaton->enabledLetters(q, LetterKind::Call)) {
      g.context->check();
      for (State t : g.automaton->successors(q, a))
        extend(2 * std::size_t(t) + 1, path, g.edge({q, a, t}));
    }
    if (!aboveBottom)
      for (Letter a :
           g.automaton->enabledLetters(q, LetterKind::Return, Bottom)) {
        g.context->check();
        for (State t : g.automaton->successors(q, a, Bottom))
          extend(2 * std::size_t(t), path, g.edge({q, a, t, Bottom}));
      }
  }
  return result;
}
} // namespace lotus::verification::automata::detail
