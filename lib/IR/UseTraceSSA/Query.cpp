#include "IR/UseTraceSSA/Query.h"
#include "IR/UseTraceSSA/QueryContext.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace lotus {
namespace usetracessa {
namespace {
using Budget = detail::SearchBudgetExceeded;
struct WitnessBudget {};
std::uint64_t key(ID a, ID b) { return (std::uint64_t(a) << 32) | b; }
ID asID(std::size_t n) {
  if (n >= InvalidID) throw std::length_error("UseTraceSSA query identifier overflow");
  return static_cast<ID>(n);
}
void bound(std::size_t n, std::size_t max, SearchStopReason reason) {
  detail::checkSearchBudget(n, max, reason);
}
struct ProductNode { FlowNodeID node; ID state; };
struct ProductEdge { ID from, to; FlowEdgeID original; };
struct ProductTransition {
  FlowNodeID node;
  ID state;
  FlowEdgeID original;
  ID edge = InvalidID;
};
enum class RecipeKind { Epsilon, Edge, Concat, Match };
struct Recipe {
  ID from, to;
  RecipeKind kind;
  ID left = InvalidID, right = InvalidID, middle = InvalidID;
  bool positive = false;
};
struct Step { ID to; ID recipe = InvalidID; ID edge = InvalidID; bool positive = false; };
} // namespace
Automaton Automaton::ordered(std::vector<Event> sequence) {
  Automaton a;
  a.states = asID(sequence.size() + 1); a.accepting = {a.states - 1};
  a.transition = [sequence = std::move(sequence)](ID state, Event event) {
    return state < sequence.size() && hasEvent(event, sequence[state]) ? state + 1 : state;
  };
  return a;
}
Automaton Automaton::doubleFree() {
  Automaton a; a.states = 3; a.accepting = {2};
  a.transition = [](ID state, Event event) -> ID {
    if (hasEvent(event, Event::Allocate)) return 0;
    if (hasEvent(event, Event::Release)) return std::min<ID>(2, state + 1);
    return state;
  };
  return a;
}
Automaton Automaton::useAfterFree() {
  Automaton a; a.states = 3; a.accepting = {2};
  a.transition = [](ID state, Event event) -> ID {
    if (hasEvent(event, Event::Allocate)) return 0;
    if (state == 2) return 2;
    if (hasEvent(event, Event::Release)) return 1;
    if (state == 1 && hasEvent(event, Event::Dereference)) return 2;
    return state;
  };
  return a;
}
namespace {
Automaton resourceLeak(Event acquire, Event release) {
  Automaton a; a.states = 3; a.accepting = {2};
  a.transition = [acquire, release](ID state, Event event) -> ID {
    if (state == 2) return 2;
    if (hasEvent(event, acquire)) return 1;
    if (hasEvent(event, release | Event::Escape)) return 0;
    if (state == 1 && hasEvent(event, Event::Exit)) return 2;
    return state;
  };
  return a;
}
} // namespace
Automaton Automaton::memoryLeak() {
  return resourceLeak(Event::Allocate, Event::Release);
}
Automaton Automaton::fileLeak() {
  return resourceLeak(Event::Open, Event::Close);
}
QueryResult QueryEngine::run(const Query &q) const {
  return runImpl(q, nullptr);
}
QueryScanResult QueryEngine::runToSinks(const Query &q) const {
  QueryScanResult scan;
  auto result = runImpl(q, &scan);
  scan.status = result.status;
  scan.completion = result.completion;
  scan.productStates = result.productStates;
  scan.edgesExamined = result.edgesExamined;
  scan.summaryPairs = result.summaryPairs;
  scan.message = std::move(result.message);
  return scan;
}
QueryResult QueryEngine::runImpl(const Query &q, QueryScanResult *scan) const {
  QueryResult result;
  if (!q.automaton.states || q.automaton.initial >= q.automaton.states)
    throw std::invalid_argument("UseTraceSSA: invalid automaton initial state");
  std::vector<bool> accepts(q.automaton.states);
  for (ID s : q.automaton.accepting) {
    if (s >= accepts.size()) throw std::invalid_argument("UseTraceSSA: invalid accepting state");
    accepts[s] = true;
  }
  std::vector<bool> sinks(G.nodes().size()), traps(G.nodes().size());
  for (auto n : q.sinks) { G.node(n); sinks[n] = true; }
  for (auto n : q.sources) G.node(n);
  for (auto n : q.traps) { G.node(n); traps[n] = true; }
  const auto sinkCount = std::count(sinks.begin(), sinks.end(), true);
  auto effects = [&](FlowNodeID id, ID state) {
    const auto &n = G.node(id);
    std::vector<ID> out;
    if (traps[id]) return out;
    // One constant object is checked everywhere: this is the old lane's
    // path-wide predicate, not independent pairwise existential overlap.
    auto effective = q.memoryObject ? G.effectiveEvent(id, *q.memoryObject) :
                                     EffectiveEvent{n.events, n.certainty};
    bool blocked = hasEvent(effective.events, q.trapEvents);
    if (!blocked) {
      ID next = q.automaton.transition ? q.automaton.transition(state, effective.events) : state;
      if (next >= q.automaton.states)
        throw std::invalid_argument("UseTraceSSA: automaton transition out of range");
      out.push_back(next);
    }
    if (effective.certainty == Certainty::May &&
        std::find(out.begin(), out.end(), state) == out.end()) out.push_back(state);
    return out;
  };
  auto validSink = [&](FlowNodeID id) {
    return q.sinkEvents == Event::None ||
           hasEvent(q.memoryObject
                        ? G.effectiveEvent(id, *q.memoryObject).events
                        : G.node(id).events,
                    q.sinkEvents);
  };
  std::vector<ProductNode> pn;
  std::vector<ProductEdge> pe;
  std::vector<std::vector<ID>> pout;
  std::vector<std::vector<ProductTransition>> transitions;
  std::vector<bool> expanded;
  std::unordered_map<detail::FlowStateKey, ID, detail::FlowStateHash> products;
  std::vector<ID> roots;
  bool incomplete = !G.complete(), limited = false, hasContext = false;
  const bool directTraversal =
      q.contextLimit || q.context == ContextMode::Insensitive;
  auto product = [&](FlowNodeID node, ID state) -> ID {
    detail::FlowStateKey k{node, state};
    auto it = products.find(k);
    if (it != products.end())
      return it->second;
    bound(pn.size(), q.maxProductStates, SearchStopReason::ProductStates);
    ID id = asID(pn.size());
    products.emplace(k, id);
    pn.push_back({node, state});
    pout.emplace_back();
    transitions.emplace_back();
    expanded.push_back(false);
    return id;
  };
  auto expand = [&](ID id) {
    if (expanded[id])
      return;
    expanded[id] = true;
    ProductNode n = pn[id];
    for (auto eid : G.outgoing(n.node)) {
      ++result.edgesExamined;
      const auto &e = G.edge(eid);
      if (!e.enabled || e.objects.empty() || (q.edgeFilter && !q.edgeFilter(e)))
        continue;
      if (q.memoryObject && !e.objects.contains(*q.memoryObject))
        continue;
      if (e.kind == FlowKind::Thread && !q.includeThreadEdges) {
        incomplete = true;
        continue;
      }
      for (auto state : effects(e.to, n.state))
        transitions[id].push_back({e.to, state, eid});
    }
  };
  auto materialize = [&](ID from, std::size_t index) -> ID {
    ProductTransition transition = transitions[from][index];
    if (transition.edge != InvalidID)
      return transition.edge;
    ID target = product(transition.node, transition.state),
       id = asID(pe.size());
    pe.push_back({from, target, transition.original});
    pout[from].push_back(id);
    transitions[from][index].edge = id;
    auto kind = G.edge(transition.original).kind;
    hasContext |= kind == FlowKind::Call || kind == FlowKind::Return;
    return id;
  };
  try {
    for (auto src : q.sources)
      for (auto state : effects(src, q.automaton.initial))
        roots.push_back(product(src, state));
    for (std::size_t i = 0; !directTraversal && i < pn.size(); ++i) {
      expand(i);
      for (std::size_t index = 0; index < transitions[i].size(); ++index)
        materialize(i, index);
    }
  } catch (const Budget &budget) {
    limited = true;
    result.completion.stop(budget.reason, budget.limit, budget.observed);
  }
  result.productStates = pn.size();

  if (directTraversal) {
    using ContextState = detail::InternedContextState;
    detail::CallStrings calls(q.contextLimit.value_or(0));
    struct ContextParent { ID previous = InvalidID, edge = InvalidID; };
    std::unordered_map<ContextState, ID, detail::InternedContextHash> known;
    std::vector<ContextState> states;
    std::vector<ContextParent> parents;
    std::deque<ID> queue;
    auto insert = [&](ContextState state, ID previous, ID edge) {
      auto found = known.find(state);
      if (found != known.end()) return;
      bound(states.size(), q.maxSummaryPairs, SearchStopReason::SummaryPairs);
      ID id = asID(states.size());
      known.emplace(state, id);
      states.push_back(state);
      parents.push_back({previous, edge});
      queue.push_back(id);
    };
    ID accepted = InvalidID;
    std::map<FlowNodeID, ID> acceptedSinks;
    try {
      for (ID root : roots)
        insert({root, 0, false, false}, InvalidID, InvalidID);
      while (!queue.empty()) {
        ID id = queue.front(); queue.pop_front();
        ContextState current = states[id];
        ProductNode productNode = pn[current.product];
        if (sinks[productNode.node] && accepts[productNode.state] &&
            validSink(productNode.node) &&
            (!q.requireNonEmpty || current.positive) &&
            (q.context != ContextMode::Balanced ||
             calls.empty(current.stack))) {
          if (accepted == InvalidID) accepted = id;
          if (!scan) break;
          acceptedSinks.emplace(productNode.node, id);
          if (acceptedSinks.size() == static_cast<std::size_t>(sinkCount)) break;
        }
        expand(current.product);
        for (std::size_t index = 0; index < transitions[current.product].size();
             ++index) {
          auto original = transitions[current.product][index].original;
          const FlowEdge &edge = G.edge(original);
          ContextState next = current;
          next.positive = q.requireNonEmpty;
          if (q.context != ContextMode::Insensitive &&
              edge.kind == FlowKind::Call) {
            next.stack = calls.push(next.stack, edge.callSite, next.truncated);
          } else if (q.context != ContextMode::Insensitive &&
                     edge.kind == FlowKind::Return) {
            if (calls.empty(next.stack)) {
              if (q.context == ContextMode::Balanced && !next.truncated) continue;
            } else {
              if (calls.top(next.stack) != edge.callSite)
                continue;
              next.stack = calls.pop(next.stack);
            }
          }
          ID edgeID = materialize(current.product, index);
          next.product = pe[edgeID].to;
          insert(next, id, edgeID);
        }
      }
    } catch (const Budget &budget) {
      limited = true;
      result.completion.stop(budget.reason, budget.limit, budget.observed);
    }
    result.summaryPairs = states.size();
    result.productStates = pn.size();
    result.completion.modelComplete = !incomplete;
    if (scan) scan->complete = result.completion.complete();
    if (accepted == InvalidID) {
      result.status = limited || incomplete ? QueryStatus::Unknown : QueryStatus::NotFound;
      result.message = limited ? "bounded-context query budget exhausted" :
                       incomplete ? "incomplete model or omitted thread edges" :
                                    "no witness in the supplied graph abstraction";
      return result;
    }
    result.status = QueryStatus::Found;
    result.message = "witness in the bounded-context abstraction";
    auto witness = [&](ID endpoint) {
      QueryResult evidence = result;
      std::vector<ID> path;
      for (ID id = endpoint; parents[id].previous != InvalidID;
           id = parents[id].previous)
        path.push_back(parents[id].edge);
      std::reverse(path.begin(), path.end());
      ID root = states[endpoint].product;
      if (!path.empty()) root = pe[path.front()].from;
      evidence.nodes.push_back(pn[root].node);
      evidence.automatonStates.push_back(pn[root].state);
      for (ID edgeID : path) {
        if (q.maxWitnessEdges && evidence.edges.size() >= q.maxWitnessEdges) {
          evidence.witnessComplete = false;
          evidence.message += "; witness rendering truncated";
          break;
        }
        const ProductEdge &edge = pe[edgeID];
        evidence.edges.push_back(edge.original);
        evidence.nodes.push_back(pn[edge.to].node);
        evidence.automatonStates.push_back(pn[edge.to].state);
      }
      return evidence;
    };
    if (scan) {
      for (const auto &sink : acceptedSinks)
        scan->foundAt.emplace(sink.first, witness(sink.second));
    } else result = witness(accepted);
    return result;
  }

  // Sparse Dyck saturation: D ::= epsilon | local | D D | call_c D return_c.
  // Recipes only refer to previously inserted recipes, forming a witness DAG.
  std::vector<Recipe> recipes;
  std::vector<std::vector<ID>> rout(pn.size());
  bool context = hasContext && q.context != ContextMode::Insensitive;
  if (context) {
    struct Fact {
      ID entry, node, recipe;
    };
    std::vector<Fact> facts;
    std::vector<std::vector<ID>> factsAt(pn.size()), callsIn(pn.size());
    std::vector<std::unordered_map<CallSiteID, std::vector<ID>>> returnsBySite(
        pn.size());
    std::unordered_map<std::uint64_t, ID> rows, matched;
    std::vector<ID> edgeRecipes(pe.size(), InvalidID);
    std::deque<ID> queue;
    auto recipe = [&](Recipe r) {
      ID id = asID(recipes.size());
      recipes.push_back(r);
      return id;
    };
    auto insert = [&](Recipe r) {
      auto k = key(r.from, r.to);
      if (rows.count(k))
        return;
      bound(facts.size() + matched.size(), q.maxSummaryPairs,
            SearchStopReason::SummaryPairs);
      ID evidence = recipe(r), id = asID(facts.size());
      rows.emplace(k, id);
      facts.push_back({r.from, r.to, evidence});
      factsAt[r.to].push_back(id);
      queue.push_back(id);
      ++result.summaryPairs;
    };
    auto match = [&](Recipe r) {
      auto k = key(r.from, r.to);
      if (matched.count(k))
        return;
      bound(facts.size() + matched.size(), q.maxSummaryPairs,
            SearchStopReason::SummaryPairs);
      ID evidence = recipe(r);
      matched.emplace(k, evidence);
      rout[r.from].push_back(evidence);
      ++result.summaryPairs;
      auto count = factsAt[r.from].size();
      for (std::size_t i = 0; i < count; ++i) {
        Fact fact = facts[factsAt[r.from][i]];
        insert({fact.entry, r.to, RecipeKind::Concat, fact.recipe, evidence,
                InvalidID, true});
      }
    };
    try {
      // Only callee entry rows are needed to derive matched-call edges. The
      // final search takes ordinary edges and those summaries directly.
      for (ID i = 0; i < pe.size(); ++i) {
        const auto &e = pe[i]; auto kind = G.edge(e.original).kind;
        if (kind == FlowKind::Call) {
          callsIn[e.to].push_back(i);
          insert({e.to, e.to, RecipeKind::Epsilon});
        } else if (kind == FlowKind::Return)
          returnsBySite[e.from][G.edge(e.original).callSite].push_back(i);
      }
      while (!queue.empty()) {
        ID id = queue.front();
        queue.pop_front();
        Fact fact = facts[id];
        for (ID edgeID : pout[fact.node]) {
          const auto &e = pe[edgeID];
          auto kind = G.edge(e.original).kind;
          if (kind == FlowKind::Call || kind == FlowKind::Return)
            continue;
          if (edgeRecipes[edgeID] == InvalidID)
            edgeRecipes[edgeID] = recipe({e.from, e.to, RecipeKind::Edge,
                                          edgeID, InvalidID, InvalidID, true});
          insert({fact.entry, e.to, RecipeKind::Concat, fact.recipe,
                  edgeRecipes[edgeID], InvalidID, true});
        }
        auto count = rout[fact.node].size();
        for (std::size_t i = 0; i < count; ++i) {
          ID summary = rout[fact.node][i];
          ID target = recipes[summary].to;
          insert({fact.entry, target, RecipeKind::Concat, fact.recipe, summary,
                  InvalidID, true});
        }
        for (ID call : callsIn[fact.entry]) {
          auto site = G.edge(pe[call].original).callSite;
          auto returns = returnsBySite[fact.node].find(site);
          if (returns == returnsBySite[fact.node].end())
            continue;
          for (auto ret : returns->second)
            match({pe[call].from, pe[ret].to, RecipeKind::Match, call, ret,
                   fact.recipe, true});
        }
      }
    } catch (const Budget &budget) {
      limited = true;
      result.completion.stop(budget.reason, budget.limit, budget.observed);
    }
  }

  // Find a path in the quotient. Realizable segments have unmatched returns
  // only before unmatched calls. Balanced summaries can occur in either phase.
  // The positive bit also allows a genuine cycle when source == sink.
  struct Parent { ID previous = InvalidID; Step step{}; };
  const std::size_t states = pn.size() * 4;
  std::vector<bool> visited(states);
  std::vector<Parent> parents(states);
  std::deque<ID> queue;
  auto encode = [&](ID p, unsigned phase, bool positive) {
    return asID(std::size_t(p) * 4 + std::size_t(phase) * 2 + unsigned(positive));
  };
  for (ID root : roots) {
    ID state = encode(root, 0, false);
    if (!visited[state]) { visited[state] = true; queue.push_back(state); }
  }
  ID accepted = InvalidID;
  std::map<FlowNodeID, ID> acceptedSinks;
  while (!queue.empty()) {
    ID s = queue.front(); queue.pop_front();
    ID p = s / 4; unsigned phase = (s % 4) / 2; bool positive = s % 2;
    if (sinks[pn[p].node] && accepts[pn[p].state] && validSink(pn[p].node) &&
        (!q.requireNonEmpty || positive)) {
      if (accepted == InvalidID) accepted = s;
      if (!scan) break;
      acceptedSinks.emplace(pn[p].node, s);
      if (acceptedSinks.size() == static_cast<std::size_t>(sinkCount)) break;
    }
    auto push = [&](Step step, unsigned nextPhase) {
      ID t = encode(step.to, nextPhase,
                    q.requireNonEmpty && (positive || step.positive));
      if (!visited[t]) {
        visited[t] = true;
        parents[t] = {s, step};
        queue.push_back(t);
      }
    };
    if (!context) {
      for (auto e : pout[p]) push({pe[e].to, InvalidID, e, true}, phase);
    } else {
      for (auto r : rout[p]) push({recipes[r].to, r, InvalidID, recipes[r].positive}, phase);
      if (q.context == ContextMode::Realizable)
        for (auto eid : pout[p]) {
          const auto &e = pe[eid]; auto kind = G.edge(e.original).kind;
          if (kind == FlowKind::Call) push({e.to, InvalidID, eid, true}, 1);
          else if (kind == FlowKind::Return && phase == 0)
            push({e.to, InvalidID, eid, true}, 0);
        }
      // Local edges are also available directly; this shortens witnesses.
      // The relation retains a positive summary as well as epsilon on cycles.
      for (auto eid : pout[p]) {
        auto kind = G.edge(pe[eid].original).kind;
        if (kind != FlowKind::Call && kind != FlowKind::Return)
          push({pe[eid].to, InvalidID, eid, true}, phase);
      }
    }
  }
  result.completion.modelComplete = !incomplete;
  if (scan) scan->complete = result.completion.complete();
  if (accepted == InvalidID) {
    result.status = limited || incomplete ? QueryStatus::Unknown : QueryStatus::NotFound;
    result.message = limited ? "query budget exhausted; absence is not established" :
                     incomplete ? "incomplete model or omitted thread edges" :
                                  "no witness in the supplied graph abstraction";
    return result;
  }
  result.status = QueryStatus::Found;
  result.message = "witness in the supplied graph abstraction; feasibility not proven";
  auto witness = [&](ID endpoint) {
    QueryResult evidence = result;
    std::vector<Step> path;
    ID state = endpoint;
    while (parents[state].previous != InvalidID) {
      path.push_back(parents[state].step); state = parents[state].previous;
    }
    std::reverse(path.begin(), path.end());
    evidence.nodes.push_back(pn[state / 4].node);
    evidence.automatonStates.push_back(pn[state / 4].state);
    auto append = [&](ID edge) {
      if (q.maxWitnessEdges && evidence.edges.size() >= q.maxWitnessEdges) throw WitnessBudget{};
      evidence.edges.push_back(pe[edge].original);
      evidence.nodes.push_back(pn[pe[edge].to].node);
      evidence.automatonStates.push_back(pn[pe[edge].to].state);
    };
    try {
      for (auto step : path) {
        std::vector<std::pair<bool, ID>> stack;
        stack.push_back({step.recipe != InvalidID,
                         step.recipe != InvalidID ? step.recipe : step.edge});
        while (!stack.empty()) {
          auto item = stack.back(); stack.pop_back();
          if (!item.first) { append(item.second); continue; }
          const auto &r = recipes[item.second];
          switch (r.kind) {
          case RecipeKind::Epsilon: break;
          case RecipeKind::Edge: append(r.left); break;
          case RecipeKind::Concat:
            stack.push_back({true, r.right}); stack.push_back({true, r.left}); break;
          case RecipeKind::Match:
            stack.push_back({false, r.right}); stack.push_back({true, r.middle});
            stack.push_back({false, r.left}); break;
          }
        }
      }
    } catch (const WitnessBudget &) {
      evidence.witnessComplete = false;
      evidence.message += "; witness rendering truncated (existence is established)";
    }
    return evidence;
  };
  if (scan) {
    for (const auto &sink : acceptedSinks)
      scan->foundAt.emplace(sink.first, witness(sink.second));
  } else result = witness(accepted);
  return result;
}
std::vector<QueryResult> QueryEngine::runBatch(const std::vector<Query> &queries) const {
  std::vector<QueryResult> results;
  for (const auto &q : queries) results.push_back(run(q));
  return results;
}
CoverageResult QueryEngine::allPathsHitTraps(const Query &query) const {
  Query plain = query; plain.traps.clear(); plain.trapEvents = Event::None;
  auto any = run(plain);
  if (any.status == QueryStatus::Unknown) return {CoverageStatus::Unknown, std::move(any)};
  if (any.status == QueryStatus::NotFound)
    return {CoverageStatus::SinkUnreachable, std::move(any)};
  auto untrapped = run(query);
  auto status = untrapped.status == QueryStatus::Found ? CoverageStatus::UntrappedPath :
                untrapped.status == QueryStatus::NotFound ? CoverageStatus::AllPathsTrapped :
                                                         CoverageStatus::Unknown;
  return {status, std::move(untrapped)};
}
std::vector<FlowNodeID> QueryEngine::slice(const std::vector<FlowNodeID> &seeds,
                                        bool backward,
                                        const std::vector<FlowNodeID> &traps) const {
  std::vector<bool> visited(G.nodes().size()), blocked(G.nodes().size());
  for (auto n : traps) { G.node(n); blocked[n] = true; }
  std::deque<FlowNodeID> queue;
  for (auto n : seeds) { G.node(n); if (!blocked[n] && !visited[n]) {
    visited[n] = true; queue.push_back(n);
  }}
  while (!queue.empty()) {
    auto n = queue.front(); queue.pop_front();
    for (auto id : backward ? G.incoming(n) : G.outgoing(n)) {
      const auto &e = G.edge(id);
      auto target = backward ? e.from : e.to;
      if (e.enabled && !e.objects.empty() && !blocked[target] && !visited[target]) {
        visited[target] = true; queue.push_back(target);
      }
    }
  }
  std::vector<FlowNodeID> result;
  for (FlowNodeID i = 0; i < visited.size(); ++i) if (visited[i]) result.push_back(i);
  return result;
}
} // namespace usetracessa
} // namespace lotus
