#include "Verification/Automata/Detail/Summary.h"

#include <algorithm>

namespace lotus::verification::automata {
bool validateLasso(SuccessorProvider &operand, const BuchiLasso &lasso,
                   Context context) {
  context->check();
  auto validShape = [&](const NestedRun &run) {
    if (run.states.size() != run.word.size() + 1 ||
        run.matching.size() != run.word.size())
      return false;
    for (auto q : run.states) {
      context->check();
      if (q >= operand.stateCount())
        return false;
    }
    for (auto a : run.word) {
      context->check();
      if (a >= operand.alphabet().size())
        return false;
    }
    NestedRun expected;
    expected.word = run.word;
    expected.computeMatching(operand.alphabet());
    return run.matching == expected.matching;
  };
  if (!validShape(lasso.stem) || !validShape(lasso.loop) ||
      lasso.loop.word.empty() ||
      lasso.stem.states.back() != lasso.loop.states.front() ||
      lasso.loop.states.front() != lasso.loop.states.back())
    return false;
  const auto &initial = operand.initialStates();
  if (std::find(initial.begin(), initial.end(), lasso.stem.states.front()) ==
      initial.end())
    return false;
  bool accepting = false;
  for (auto q : lasso.loop.states) {
    context->check();
    accepting = accepting || operand.isFinal(q);
  }
  if (!accepting)
    return false;
  std::vector<State> stack;
  std::size_t capacity = context->maxPairs;
  if (lasso.stem.word.size() <= capacity &&
      lasso.loop.word.size() <= (capacity - lasso.stem.word.size()) / 2)
    capacity = lasso.stem.word.size() + 2 * lasso.loop.word.size();
  stack.reserve(capacity);
  auto replay = [&](const NestedRun &run, bool loop) {
    const auto base = stack.size();
    for (std::size_t i = 0; i < run.word.size(); ++i) {
      context->check();
      const auto kind = operand.alphabet().kind(run.word[i]);
      State hierarchy = Bottom;
      if (kind == LetterKind::Call) {
        if (stack.size() >= context->maxPairs)
          throw ResourceLimit();
        stack.push_back(run.states[i]);
      } else if (kind == LetterKind::Return && !stack.empty()) {
        // The loop certificate must not depend on frames from the stem or a
        // previous iteration: otherwise two executions do not prove pumping.
        if (loop && stack.size() <= base)
          return false;
        hierarchy = stack.back();
        stack.pop_back();
      }
      const auto &targets =
          operand.successors(run.states[i], run.word[i], hierarchy);
      if (std::find(targets.begin(), targets.end(), run.states[i + 1]) ==
          targets.end())
        return false;
    }
    return !loop || stack.size() - base == lasso.stackGrowth;
  };
  return replay(lasso.stem, false) && replay(lasso.loop, true) &&
         replay(lasso.loop, true);
}

BuchiEmptinessResult buchiIsEmpty(Provider operand, Context context) {
  auto reach = reachable(std::move(operand), context);
  detail::SummaryGraph g(reach.automaton, {}, context);
  g.compute();
  auto prefixes = detail::prefixes(g, false);
  const auto count = 2 * g.automaton->stateCount();
  struct Edge {
    std::size_t target;
    detail::PathId path;
  };
  std::vector<std::vector<Edge>> edges(count);
  for (std::size_t v = 0; v < count; ++v) {
    context->check();
    if (prefixes.paths[v] == detail::NoPath)
      continue;
    State q = static_cast<State>(v / 2);
    for (auto id : g.outgoing[q]) {
      context->check();
      auto s = g.summaries[id];
      if (g.paths[s.path].nonempty)
        edges[v].push_back({2 * std::size_t(s.target) + v % 2, s.path});
    }
    for (Letter a : g.automaton->enabledLetters(q, LetterKind::Call)) {
      context->check();
      for (State t : g.automaton->successors(q, a))
        edges[v].push_back({2 * std::size_t(t) + 1, g.edge({q, a, t})});
    }
    if (v % 2 == 0)
      for (Letter a :
           g.automaton->enabledLetters(q, LetterKind::Return, Bottom)) {
        context->check();
        for (State t : g.automaton->successors(q, a, Bottom))
          edges[v].push_back({2 * std::size_t(t), g.edge({q, a, t, Bottom})});
      }
  }

  // Iterative Tarjan: neither recursion depth nor native stack size bounds the
  // automaton. Summary edges carry whether an accepting state was visited.
  const auto none = detail::NoPath;
  std::vector<std::size_t> index(count, none), low(count),
      component(count, none);
  std::vector<bool> onStack(count);
  std::vector<std::size_t> stack;
  stack.reserve(count);
  struct Frame {
    std::size_t vertex, next;
  };
  std::vector<Frame> dfs;
  dfs.reserve(count);
  std::size_t nextIndex = 0, nextComponent = 0;
  {
    detail::HotLoopScope hot(*context, "buchi-tarjan");
    for (std::size_t root = 0; root < count; ++root) {
      context->check();
      if (index[root] != none || prefixes.paths[root] == none)
        continue;
      auto enter = [&](std::size_t v) {
        index[v] = low[v] = nextIndex++;
        stack.push_back(v);
        onStack[v] = true;
        dfs.push_back({v, 0});
      };
      enter(root);
      while (!dfs.empty()) {
        context->check();
        auto v = dfs.back().vertex;
        if (dfs.back().next < edges[v].size()) {
          auto w = edges[v][dfs.back().next++].target;
          if (index[w] == none)
            enter(w);
          else if (onStack[w])
            low[v] = std::min(low[v], index[w]);
        } else {
          if (low[v] == index[v]) {
            std::size_t w;
            do {
              context->check();
              w = stack.back();
              stack.pop_back();
              onStack[w] = false;
              component[w] = nextComponent;
            } while (w != v);
            ++nextComponent;
          }
          dfs.pop_back();
          if (!dfs.empty()) {
            auto parent = dfs.back().vertex;
            low[parent] = std::min(low[parent], low[v]);
          }
        }
      }
    }
  }
  for (std::size_t source = 0; source < count; ++source) {
    context->check();
    for (const auto &edge : edges[source]) {
      context->check();
      if (component[source] != component[edge.target] ||
          !g.paths[edge.path].accepting)
        continue;
      // Find a route back within the SCC, then expand the witness DAG once.
      std::vector<std::size_t> previous(count, none), queue;
      queue.reserve(count);
      queue.push_back(edge.target);
      std::vector<detail::PathId> via(count, none);
      previous[edge.target] = edge.target;
      {
        detail::HotLoopScope hot(*context, "buchi-lasso-path");
        for (std::size_t head = 0;
             head < queue.size() && previous[source] == none; ++head) {
          context->check();
          auto v = queue[head];
          for (const auto &e : edges[v]) {
            context->check();
            if (component[e.target] == component[source] &&
                previous[e.target] == none) {
              previous[e.target] = v;
              via[e.target] = e.path;
              queue.push_back(e.target);
            }
          }
        }
      }
      auto loopPath = edge.path;
      std::vector<detail::PathId> reverse;
      reverse.reserve(count);
      {
        detail::HotLoopScope hot(*context, "buchi-lasso-reconstruction");
        for (auto v = source; v != edge.target; v = previous[v]) {
          context->check();
          reverse.push_back(via[v]);
        }
        for (auto it = reverse.rbegin(); it != reverse.rend(); ++it)
          loopPath = g.concat(loopPath, *it);
      }
      BuchiLasso lasso{g.expand(prefixes.paths[source]), g.expand(loopPath), 0};
      for (std::size_t i = 0; i < lasso.loop.word.size(); ++i) {
        context->check();
        if (g.automaton->alphabet().kind(lasso.loop.word[i]) ==
                LetterKind::Call &&
            lasso.loop.matching[i] == -1)
          ++lasso.stackGrowth;
      }
      for (auto *run : {&lasso.stem, &lasso.loop})
        for (auto &q : run->states)
          q = reach.originalStates[q];
      return {std::move(lasso)};
    }
  }
  return {};
}
} // namespace lotus::verification::automata
