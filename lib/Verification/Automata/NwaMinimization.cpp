#include "Verification/Automata/Detail/Workspace.h"
#include "Verification/Automata/NwaReduction.h"

#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <tuple>

namespace lotus::verification::automata {
namespace {
using Label = std::tuple<unsigned, State, Letter>;
using Edges = std::map<Label, std::vector<State>>;
std::vector<Edges> simulationEdges(const NestedWordAutomaton &a,
                                   Context context) {
  std::vector<Edges> result(a.stateCount());
  for (const auto &t : a.transitions()) {
    context->check();
    if (a.alphabet().kind(t.letter) == LetterKind::Return) {
      result[t.source][{1, t.hierarchy, t.letter}].push_back(t.target);
      if (t.hierarchy != Bottom)
        result[t.hierarchy][{2, t.source, t.letter}].push_back(t.target);
    } else {
      result[t.source][{0, Bottom, t.letter}].push_back(t.target);
    }
  }
  return result;
}
void clearPair(SimulationRelation &r, State p, State q) {
  auto bit = std::size_t(p) * r.states + q;
  r.bits[bit / 64] &= ~(std::uint64_t(1) << (bit % 64));
}
std::shared_ptr<NestedWordAutomaton> quotient(const NestedWordAutomaton &a,
                                              const std::vector<State> &classes,
                                              Context context) {
  auto result = std::make_shared<NestedWordAutomaton>(a.alphabet());
  State count = classes.empty()
                    ? 0
                    : *std::max_element(classes.begin(), classes.end()) + 1;
  std::vector<bool> initial(count), final(count);
  for (State q : a.initialStates())
    initial[classes[q]] = true;
  for (State q = 0; q < a.stateCount(); ++q) {
    context->check();
    final[classes[q]] = final[classes[q]] || a.isFinal(q);
  }
  for (State q = 0; q < count; ++q)
    result->addState(initial[q], final[q]);
  for (const auto &t : a.transitions()) {
    context->check();
    result->addTransition(classes[t.source], t.letter, classes[t.target],
                          t.hierarchy == Bottom ? Bottom
                                                : classes[t.hierarchy]);
  }
  return result;
}

std::shared_ptr<NestedWordAutomaton>
bisimulationQuotient(const NestedWordAutomaton &a, Context context) {
  auto edges = simulationEdges(a, context);
  const auto n = a.stateCount();
  std::vector<State> classes(n), next(n), order(n);
  std::iota(order.begin(), order.end(), 0);
  std::vector<std::vector<std::array<std::uint64_t, 3>>> signatures(n);
  bool hasFinal = false, hasNonfinal = false;
  for (State q = 0; q < n; ++q) {
    context->check();
    classes[q] = a.isFinal(q) ? 1 : 0;
    hasFinal = hasFinal || a.isFinal(q);
    hasNonfinal = hasNonfinal || !a.isFinal(q);
    std::size_t capacity = 0;
    for (const auto &entry : edges[q])
      capacity += entry.second.size();
    signatures[q].reserve(capacity);
  }
  std::size_t count = unsigned(hasFinal) + unsigned(hasNonfinal);
  {
    detail::HotLoopScope hot(*context, "bisimulation-refinement");
    while (n) {
      context->check();
      for (State q = 0; q < n; ++q) {
        context->check();
        auto &signature = signatures[q];
        signature.clear();
        for (const auto &entry : edges[q]) {
          context->check();
          const auto [role, hierarchy, letter] = entry.first;
          for (State target : entry.second) {
            context->check();
            signature.push_back({role,
                                 (std::uint64_t(hierarchy) << 32) | letter,
                                 classes[target]});
          }
        }
        detail::checkedSort(signature.begin(), signature.end(), *context);
        signature.erase(
            detail::checkedUnique(signature.begin(), signature.end(), *context),
            signature.end());
      }
      std::sort(order.begin(), order.end(), [&](State p, State q) {
        context->check();
        return classes[p] != classes[q] ? classes[p] < classes[q]
                                        : signatures[p] < signatures[q];
      });
      State nextCount = 1;
      next[order[0]] = 0;
      for (std::size_t i = 1; i < n; ++i) {
        context->check();
        State p = order[i - 1], q = order[i];
        if (classes[p] != classes[q] || signatures[p] != signatures[q])
          ++nextCount;
        next[q] = nextCount - 1;
      }
      classes.swap(next);
      if (count == nextCount)
        break;
      count = nextCount;
    }
  }
  return quotient(a, classes, context);
}
} // namespace

bool SimulationRelation::contains(State p, State q) const {
  if (p >= states || q >= states)
    throw std::out_of_range("simulation state");
  auto bit = std::size_t(p) * states + q;
  return (bits.at(bit / 64) >> (bit % 64)) & 1U;
}
SimulationRelation directSimulation(const NestedWordAutomaton &a,
                                    Context context) {
  context->check();
  const auto n = a.stateCount();
  if (n && n > context->maxPairs / n)
    throw ResourceLimit();
  SimulationRelation relation{
      n, std::vector<std::uint64_t>((n * n + 63) / 64, ~std::uint64_t(0))};
  auto edges = simulationEdges(a, context);
  for (State p = 0; p < n; ++p)
    for (State q = 0; q < n; ++q) {
      context->check();
      if (a.isFinal(p) && !a.isFinal(q))
        clearPair(relation, p, q);
    }
  detail::HotLoopScope hot(*context, "context-direct-simulation");
  bool changed = true;
  while (changed) {
    context->check();
    changed = false;
    for (State p = 0; p < n; ++p) {
      context->check();
      for (State q = 0; q < n; ++q) {
        context->check();
        if (!relation.contains(p, q))
          continue;
        bool valid = true;
        for (const auto &edge : edges[p]) {
          context->check();
          auto response = edges[q].find(edge.first);
          if (response == edges[q].end()) {
            valid = false;
            break;
          }
          for (State pNext : edge.second) {
            context->check();
            bool match = false;
            for (State qNext : response->second) {
              context->check();
              if (relation.contains(pNext, qNext)) {
                match = true;
                break;
              }
            }
            if (!match) {
              valid = false;
              break;
            }
          }
          if (!valid)
            break;
        }
        if (!valid) {
          clearPair(relation, p, q);
          changed = true;
        }
      }
    }
  }
  return relation;
}
std::shared_ptr<NestedWordAutomaton>
reduceDirectSimulation(const NestedWordAutomaton &a, Context context) {
  // Bisimulation is a subset of mutual direct simulation. Quotienting it first
  // avoids constructing a quadratic game for thousands of duplicate products.
  auto compact = bisimulationQuotient(a, context);
  auto relation = directSimulation(*compact, context);
  std::vector<State> classes(compact->stateCount(), Bottom);
  State nextClass = 0;
  for (State p = 0; p < compact->stateCount(); ++p) {
    context->check();
    if (classes[p] != Bottom)
      continue;
    classes[p] = nextClass;
    for (State q = p + 1; q < compact->stateCount(); ++q) {
      context->check();
      if (relation.contains(p, q) && relation.contains(q, p))
        classes[q] = nextClass;
    }
    ++nextClass;
  }
  return quotient(*compact, classes, context);
}

std::shared_ptr<NestedWordAutomaton> minimizeDfa(const NestedWordAutomaton &a,
                                                 Context context) {
  context->check();
  if (a.initialStates().size() > 1)
    throw std::invalid_argument(
        "DFA minimization requires at most one initial state");
  const auto n = a.stateCount();
  const auto &letters = a.alphabet().letters(LetterKind::Internal);
  const auto m = letters.size();
  if (n >= Bottom || (m && n + 1 > context->maxPairs / m))
    throw ResourceLimit();
  State sink = static_cast<State>(n);
  std::unordered_map<Letter, std::size_t> letterIndex;
  for (std::size_t i = 0; i < m; ++i)
    letterIndex.emplace(letters[i], i);
  std::vector<std::vector<State>> delta(n + 1, std::vector<State>(m, sink));
  for (const auto &t : a.transitions()) {
    context->check();
    if (a.alphabet().kind(t.letter) != LetterKind::Internal)
      throw std::invalid_argument(
          "DFA minimization does not support calls or returns");
    auto &target = delta[t.source][letterIndex.at(t.letter)];
    if (target != sink && target != t.target)
      throw std::invalid_argument(
          "DFA minimization requires deterministic transitions");
    target = t.target;
  }
  std::vector<std::vector<std::vector<State>>> predecessors(
      m, std::vector<std::vector<State>>(n + 1));
  for (State q = 0; q <= n; ++q)
    for (std::size_t l = 0; l < m; ++l) {
      context->check();
      predecessors[l][delta[q][l]].push_back(q);
    }
  detail::Workspace workspace(context->maxWorkspaceBytes);
  std::pmr::vector<std::pmr::vector<State>> blocks(workspace.resource());
  blocks.resize(2);
  std::vector<State> blockOf(n + 1);
  for (State q = 0; q <= n; ++q) {
    context->check();
    State block = q != sink && a.isFinal(q) ? 1 : 0;
    blocks[block].push_back(q);
    blockOf[q] = block;
  }
  std::pmr::vector<std::pmr::vector<bool>> scheduled(workspace.resource());
  scheduled.resize(2);
  for (auto &row : scheduled)
    row.resize(m, false);
  std::pmr::vector<std::pair<State, std::size_t>> work(workspace.resource());
  auto schedule = [&](State b, std::size_t l) {
    if (!scheduled[b][l]) {
      scheduled[b][l] = true;
      work.emplace_back(b, l);
    }
  };
  for (std::size_t l = 0; l < m; ++l)
    schedule(blocks[0].size() < blocks[1].size() ? 0 : 1, l);
  std::vector<bool> marked(n + 1);
  std::pmr::vector<State> touchedStates(workspace.resource()),
      touchedBlocks(workspace.resource());
  std::pmr::vector<std::size_t> hits(2, 0, workspace.resource());
  {
    detail::HotLoopScope hot(*context, "hopcroft-refinement");
    for (std::size_t head = 0; head < work.size(); ++head) {
      context->check();
      auto [splitter, letter] = work[head];
      scheduled[splitter][letter] = false;
      touchedStates.clear();
      touchedBlocks.clear();
      for (State target : blocks[splitter]) {
        context->check();
        for (State source : predecessors[letter][target]) {
          context->check();
          if (!marked[source]) {
            marked[source] = true;
            touchedStates.push_back(source);
            auto block = blockOf[source];
            if (hits[block]++ == 0)
              touchedBlocks.push_back(block);
          }
        }
      }
      for (State block : touchedBlocks) {
        context->check();
        if (hits[block] != blocks[block].size()) {
          State fresh = static_cast<State>(blocks.size());
          std::pmr::vector<State> kept(workspace.resource()),
              moved(workspace.resource());
          kept.reserve(blocks[block].size() - hits[block]);
          moved.reserve(hits[block]);
          for (State q : blocks[block]) {
            context->check();
            (marked[q] ? moved : kept).push_back(q);
          }
          blocks[block] = std::move(kept);
          blocks.push_back(std::move(moved));
          scheduled.emplace_back(m, false);
          hits.push_back(0);
          for (State q : blocks[fresh])
            blockOf[q] = fresh;
          for (std::size_t l = 0; l < m; ++l) {
            context->check();
            if (scheduled[block][l])
              schedule(fresh, l);
            else
              schedule(blocks[block].size() < blocks[fresh].size() ? block
                                                                   : fresh,
                       l);
          }
        }
        hits[block] = 0;
      }
      for (State q : touchedStates)
        marked[q] = false;
    }
  }
  std::vector<State> classes(n), ids(blocks.size(), Bottom);
  State nextClass = 0;
  for (State q = 0; q < n; ++q) {
    context->check();
    auto block = blockOf[q];
    if (ids[block] == Bottom)
      ids[block] = nextClass++;
    classes[q] = ids[block];
  }
  // Complete the quotient consistently. Taking the union of partial rows can
  // otherwise introduce apparent nondeterminism in the sink-equivalent block.
  auto result = std::make_shared<NestedWordAutomaton>(a.alphabet());
  for (State q = 0; q < nextClass; ++q)
    result->addState(!a.initialStates().empty() &&
                         classes[a.initialStates()[0]] == q,
                     false);
  for (State q = 0; q < n; ++q) {
    context->check();
    if (a.isFinal(q))
      result->setFinal(classes[q], true);
    for (std::size_t l = 0; l < m; ++l) {
      context->check();
      State targetBlock = blockOf[delta[q][l]];
      if (ids[targetBlock] != Bottom)
        result->addTransition(classes[q], letters[l], ids[targetBlock]);
    }
  }
  return reachable(result, context).automaton;
}

SimulationRelation delayedSimulation(const NestedWordAutomaton &a,
                                     AcceptanceSemantics semantics,
                                     Context context) {
  if (semantics == AcceptanceSemantics::Finite)
    return directSimulation(a, std::move(context));
  context->check();
  const auto n = a.stateCount();
  if (n && n > (context->maxPairs / 2) / n)
    throw ResourceLimit();
  auto allEdges = simulationEdges(a, context);
  std::vector<Edges> rows(n), columns(n);
  for (State q = 0; q < n; ++q) {
    context->check();
    for (auto &entry : allEdges[q]) {
      context->check();
      detail::checkedSort(entry.second.begin(), entry.second.end(), *context);
      entry.second.erase(detail::checkedUnique(entry.second.begin(),
                                               entry.second.end(), *context),
                         entry.second.end());
      (std::get<0>(entry.first) == 2 ? columns[q] : rows[q]).emplace(entry);
    }
  }
  struct Vertex {
    bool duplicator = false;
    bool accepting = false;
    std::vector<std::size_t> successors;
  };
  std::vector<Vertex> game(2 * n * n + 2);
  const auto win = 2 * n * n, lose = win + 1;
  game[win].accepting = true;
  game[win].successors.push_back(win);
  game[lose].successors.push_back(lose);
  auto vertex = [n](State p, State q, bool bit) {
    return 2 * (std::size_t(p) * n + q) + bit;
  };
  for (State p = 0; p < n; ++p) {
    context->check();
    for (State q = 0; q < n; ++q) {
      context->check();
      for (unsigned bit = 0; bit < 2; ++bit) {
        context->check();
        auto id = vertex(p, q, bit);
        game[id].accepting = !bit;
        if (columns[p] != columns[q]) {
          game[id].successors.push_back(lose);
          continue;
        }
        if (rows[p].empty()) {
          // Spoiler cannot construct an infinite run from this position.
          game[id].successors.push_back(win);
          continue;
        }
        for (const auto &edge : rows[p]) {
          context->check();
          auto responses = rows[q].find(edge.first);
          for (State pNext : edge.second) {
            context->check();
            if (game.size() >= context->maxPairs)
              throw ResourceLimit();
            auto challenge = game.size();
            game[id].successors.push_back(challenge);
            game.push_back({true, false, {}});
            if (responses == rows[q].end()) {
              game[challenge].successors.push_back(lose);
            } else {
              for (State qNext : responses->second) {
                context->check();
                bool nextBit = (bit || a.isFinal(pNext)) && !a.isFinal(qNext);
                game[challenge].successors.push_back(
                    vertex(pNext, qNext, nextBit));
              }
            }
          }
        }
      }
    }
  }
  // Solve the Büchi game using nu Z . mu Y .
  // ((F intersect CPre(Z)) union CPre(Y)). Attractors use reverse adjacency
  // and reusable queue/counter buffers, rather than repeated whole-graph scans.
  const auto size = game.size();
  std::vector<std::vector<std::size_t>> predecessors(size);
  for (std::size_t v = 0; v < size; ++v) {
    context->check();
    for (auto t : game[v].successors)
      predecessors[t].push_back(v);
  }
  std::vector<bool> z(size, true), y(size);
  std::vector<std::size_t> remaining(size), queue;
  queue.reserve(size);
  {
    detail::HotLoopScope hot(*context, "context-delayed-simulation");
    bool changed = true;
    while (changed) {
      context->check();
      std::fill(y.begin(), y.end(), false);
      queue.clear();
      for (std::size_t v = 0; v < size; ++v) {
        context->check();
        remaining[v] = game[v].successors.size();
        if (!game[v].accepting)
          continue;
        bool pre = !game[v].duplicator;
        for (auto t : game[v].successors) {
          context->check();
          if (game[v].duplicator)
            pre = pre || z[t];
          else
            pre = pre && z[t];
        }
        if (pre) {
          y[v] = true;
          queue.push_back(v);
        }
      }
      for (std::size_t head = 0; head < queue.size(); ++head) {
        context->check();
        for (auto v : predecessors[queue[head]]) {
          context->check();
          if (y[v])
            continue;
          if (game[v].duplicator || --remaining[v] == 0) {
            y[v] = true;
            queue.push_back(v);
          }
        }
      }
      changed = z != y;
      z.swap(y);
    }
  }
  SimulationRelation relation{n, std::vector<std::uint64_t>((n * n + 63) / 64)};
  for (State p = 0; p < n; ++p)
    for (State q = 0; q < n; ++q) {
      context->check();
      if (z[vertex(p, q, a.isFinal(p) && !a.isFinal(q))]) {
        auto bit = std::size_t(p) * n + q;
        relation.bits[bit / 64] |= std::uint64_t(1) << (bit % 64);
      }
    }
  return relation;
}

std::shared_ptr<NestedWordAutomaton>
reduceDelayedSimulation(const NestedWordAutomaton &a,
                        AcceptanceSemantics semantics, Context context) {
  auto relation = delayedSimulation(a, semantics, context);
  std::vector<State> classes(a.stateCount(), Bottom);
  State nextClass = 0;
  for (State p = 0; p < a.stateCount(); ++p) {
    context->check();
    if (classes[p] != Bottom)
      continue;
    classes[p] = nextClass;
    for (State q = p + 1; q < a.stateCount(); ++q) {
      context->check();
      if (relation.contains(p, q) && relation.contains(q, p))
        classes[q] = nextClass;
    }
    ++nextClass;
  }
  return quotient(a, classes, context);
}
} // namespace lotus::verification::automata
