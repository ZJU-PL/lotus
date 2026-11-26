#include "IR/UFG/Search.h"
#include "IR/UseTraceSSA/QueryContext.h"

#include <algorithm>
#include <deque>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace lotus {
namespace ufg {
using namespace usetracessa;
using ID = SearchID;
using StateID = usetracessa::ID;
constexpr ID InvalidID = std::numeric_limits<ID>::max();
namespace {
using Budget = usetracessa::detail::SearchBudgetExceeded;
struct WitnessBudget {};
struct Product { FlowNodeID node; StateID state; };
struct ProductEdge { ID from, to; FlowEdgeID edge; };
enum class ProofKind { Empty, Edge, Concat, Match };
struct Fact {
  ID from, to;
  bool positive;
  ProofKind kind;
  ID first = InvalidID, second = InvalidID, third = InvalidID;
};
struct Parent {
  ID previous = InvalidID;
  bool fact = false;
  ID item = InvalidID;
};
struct FactKey {
  ID from, to;
  bool positive;
  bool operator==(const FactKey &other) const {
    return from == other.from && to == other.to && positive == other.positive;
  }
};
struct FactHash {
  std::size_t operator()(FactKey fact) const {
    return llvm::hash_combine(fact.from, fact.to, fact.positive);
  }
};

ID checked(std::size_t size) {
  if (size >= InvalidID) throw std::length_error("UFG: search identifier space exhausted");
  return static_cast<ID>(size);
}
void bound(std::size_t size, std::size_t limit, SearchStopReason reason) {
  usetracessa::detail::checkSearchBudget(size, limit, reason);
}
} // namespace

LaneSearchResult SearchEngine::scan(Query query, ObjectID object) const {
  if (query.memoryObject && *query.memoryObject != object)
    throw std::invalid_argument("UFG: conflicting fixed object");
  query.memoryObject.reset();
  auto mapNodes = [&](std::vector<FlowNodeID> &nodes) {
    for (auto &node : nodes) node = Graph.node(object, node);
  };
  mapNodes(query.sources);
  mapNodes(query.sinks);
  mapNodes(query.traps);
  return tabulate(std::move(query), Graph.graph(), true);
}

LaneSearchResult SearchEngine::scanGeneric(Query query) const {
  if (query.memoryObject)
    throw std::invalid_argument("UFG: generic query cannot fix a resource object");
  return tabulate(std::move(query), Graph.source(), false);
}

LaneSearchResult SearchEngine::tabulate(Query query, const TraceFlowGraph &graph,
                                        bool expanded) const {
  if (!query.automaton.states || query.automaton.initial >= query.automaton.states)
    throw std::invalid_argument("UFG: invalid automaton initial state");
  std::vector<bool> accepts(query.automaton.states);
  for (StateID state : query.automaton.accepting) {
    if (state >= accepts.size()) throw std::invalid_argument("UFG: invalid accepting state");
    accepts[state] = true;
  }
  std::unordered_set<FlowNodeID> sinks, traps;
  for (auto node : query.sinks) { graph.node(node); sinks.insert(node); }
  for (auto node : query.traps) { graph.node(node); traps.insert(node); }

  LaneSearchResult result;
  bool incomplete = !graph.complete(), limited = false;
  auto advance = [&](FlowNodeID node, StateID state) {
    std::vector<StateID> successors;
    if (traps.count(node)) return successors;
    const auto &effect = graph.node(node);
    if (!hasEvent(effect.events, query.trapEvents)) {
      StateID next = query.automaton.transition ?
          query.automaton.transition(state, effect.events) : state;
      if (next >= query.automaton.states)
        throw std::invalid_argument("UFG: automaton transition out of range");
      successors.push_back(next);
    }
    if (effect.certainty == Certainty::May &&
        std::find(successors.begin(), successors.end(), state) == successors.end())
      successors.push_back(state);
    return successors;
  };

  std::vector<Product> products;
  std::vector<ProductEdge> edges;
  std::vector<std::vector<ID>> outgoing;
  std::unordered_map<usetracessa::detail::FlowStateKey, ID,
                     usetracessa::detail::FlowStateHash> productIDs;
  std::vector<ID> roots;
  bool hasContext = false;
  auto product = [&](FlowNodeID node, StateID state) {
    usetracessa::detail::FlowStateKey key{node, state};
    auto found = productIDs.find(key);
    if (found != productIDs.end()) return found->second;
    bound(products.size(), query.maxProductStates, SearchStopReason::ProductStates);
    ID id = checked(products.size());
    productIDs.emplace(key, id);
    products.push_back({node, state});
    outgoing.emplace_back();
    return id;
  };
  try {
    for (auto source : query.sources)
      for (StateID state : advance(source, query.automaton.initial))
        roots.push_back(product(source, state));
    for (std::size_t i = 0; i < products.size(); ++i) {
      Product current = products[i];
      for (auto eid : graph.outgoing(current.node)) {
        ++result.edgesExamined;
        const FlowEdge &edge = graph.edge(eid);
        if (!edge.enabled || edge.objects.empty() ||
            (query.edgeFilter && !query.edgeFilter(edge))) continue;
        if (edge.kind == FlowKind::Thread && !query.includeThreadEdges) {
          incomplete = true;
          continue;
        }
        for (StateID state : advance(edge.to, current.state)) {
          ID target = product(edge.to, state);
          ID id = checked(edges.size());
          edges.push_back({checked(i), target, eid});
          outgoing[i].push_back(id);
          hasContext |= edge.kind == FlowKind::Call || edge.kind == FlowKind::Return;
        }
      }
    }
  } catch (const Budget &budget) {
    limited = true;
    result.completion.stop(budget.reason, budget.limit, budget.observed);
  }
  result.productStates = products.size();
  result.productEdges = edges.size();

  if (query.contextLimit && query.context != ContextMode::Insensitive) {
    struct ContextState {
      ID product;
      std::vector<CallSiteID> calls;
      bool truncated = false;
      bool positive = false;
    };
    struct ContextParent { ID previous = InvalidID, edge = InvalidID; };
    std::map<std::tuple<ID, std::vector<CallSiteID>, bool, bool>, ID> known;
    std::vector<ContextState> states;
    std::vector<ContextParent> parents;
    std::deque<ID> queue;
    auto insert = [&](ContextState state, ID previous, ID edge) {
      auto identity = std::make_tuple(state.product, state.calls,
                                      state.truncated, state.positive);
      if (known.count(identity)) return;
      bound(states.size(), query.maxSummaryPairs, SearchStopReason::SummaryPairs);
      ID id = checked(states.size());
      known.emplace(std::move(identity), id);
      states.push_back(std::move(state));
      parents.push_back({previous, edge});
      queue.push_back(id);
    };
    std::map<FlowNodeID, ID> accepted;
    try {
      for (ID root : roots) insert({root, {}, false, false}, InvalidID, InvalidID);
      while (!queue.empty()) {
        ID id = queue.front(); queue.pop_front();
        ContextState current = states[id];
        Product point = products[current.product];
        if (sinks.count(point.node) && accepts[point.state] &&
            (!query.requireNonEmpty || current.positive) &&
            (query.context != ContextMode::Balanced || current.calls.empty()))
          accepted.emplace(expanded ? Graph.originalNode(point.node) : point.node, id);
        for (ID edgeID : outgoing[current.product]) {
          const ProductEdge &step = edges[edgeID];
          const FlowEdge &edge = graph.edge(step.edge);
          ContextState next = current;
          next.product = step.to;
          next.positive = true;
          if (edge.kind == FlowKind::Call) {
            if (next.calls.size() >= *query.contextLimit) {
              if (!next.calls.empty()) next.calls.erase(next.calls.begin());
              next.truncated = true;
            }
            next.calls.push_back(edge.callSite);
          } else if (edge.kind == FlowKind::Return) {
            if (next.calls.empty()) {
              if (query.context == ContextMode::Balanced && !next.truncated) continue;
            } else {
              if (next.calls.back() != edge.callSite) continue;
              next.calls.pop_back();
            }
          }
          insert(std::move(next), id, edgeID);
        }
      }
    } catch (const Budget &budget) {
      limited = true;
      result.completion.stop(budget.reason, budget.limit, budget.observed);
    }
    result.summaryPairs = states.size();
    result.completion.modelComplete = !incomplete;
    result.exhaustive = result.completion.complete();
    result.status = !accepted.empty() ? QueryStatus::Found :
                    result.exhaustive ? QueryStatus::NotFound : QueryStatus::Unknown;
    result.message = limited ? "bounded-context tabulation budget exhausted" :
                     incomplete ? "incomplete model or omitted thread edges" :
                                  "bounded-context lane tabulation complete";
    for (const auto &hit : accepted) {
      QueryResult witness;
      witness.completion = result.completion;
      witness.status = QueryStatus::Found;
      witness.productStates = result.productStates;
      witness.edgesExamined = result.edgesExamined;
      witness.summaryPairs = result.summaryPairs;
      witness.message = "witness in the bounded-context UFG abstraction";
      std::vector<ID> path;
      for (ID id = hit.second; parents[id].previous != InvalidID;
           id = parents[id].previous)
        path.push_back(parents[id].edge);
      std::reverse(path.begin(), path.end());
      ID root = states[hit.second].product;
      if (!path.empty()) root = edges[path.front()].from;
      witness.nodes.push_back(products[root].node);
      witness.automatonStates.push_back(products[root].state);
      for (ID edgeID : path) {
        if (query.maxWitnessEdges && witness.edges.size() >= query.maxWitnessEdges) {
          witness.witnessComplete = false;
          witness.message += "; witness rendering truncated";
          break;
        }
        const ProductEdge &step = edges[edgeID];
        witness.edges.push_back(step.edge);
        witness.nodes.push_back(products[step.to].node);
        witness.automatonStates.push_back(products[step.to].state);
      }
      result.foundAt.emplace(hit.first, std::move(witness));
    }
    return result;
  }

  // Balanced path facts: epsilon, local, concatenation, and matched call/return.
  // Every fact stores an acyclic proof for lazy witness reconstruction.
  const bool contextual = hasContext && query.context != ContextMode::Insensitive;
  std::vector<Fact> facts;
  std::vector<std::vector<ID>> factsOut(products.size()), factsIn(products.size());
  if (contextual) {
    std::unordered_map<FactKey, ID, FactHash> known;
    std::deque<ID> worklist;
    std::vector<std::vector<ID>> callsIn(products.size()), returnsOut(products.size());
    auto insert = [&](Fact fact) {
      FactKey identity{fact.from, fact.to, fact.positive};
      if (known.count(identity)) return;
      bound(facts.size(), query.maxSummaryPairs, SearchStopReason::SummaryPairs);
      ID id = checked(facts.size());
      known.emplace(identity, id);
      facts.push_back(fact);
      factsOut[fact.from].push_back(id);
      factsIn[fact.to].push_back(id);
      worklist.push_back(id);
    };
    try {
      for (ID i = 0; i < products.size(); ++i)
        insert({i, i, false, ProofKind::Empty});
      for (ID i = 0; i < edges.size(); ++i) {
        const auto &edge = edges[i];
        FlowKind kind = graph.edge(edge.edge).kind;
        if (kind == FlowKind::Call) callsIn[edge.to].push_back(i);
        else if (kind == FlowKind::Return) returnsOut[edge.from].push_back(i);
        else insert({edge.from, edge.to, true, ProofKind::Edge, i});
      }
      while (!worklist.empty()) {
        ID id = worklist.front(); worklist.pop_front();
        Fact fact = facts[id];
        const std::size_t predecessors = factsIn[fact.from].size();
        for (std::size_t i = 0; i < predecessors; ++i) {
          ID left = factsIn[fact.from][i];
          Fact previous = facts[left];
          insert({previous.from, fact.to, previous.positive || fact.positive,
                  ProofKind::Concat, left, id});
        }
        const std::size_t successors = factsOut[fact.to].size();
        for (std::size_t i = 0; i < successors; ++i) {
          ID right = factsOut[fact.to][i];
          Fact next = facts[right];
          insert({fact.from, next.to, fact.positive || next.positive,
                  ProofKind::Concat, id, right});
        }
        for (ID call : callsIn[fact.from])
          for (ID ret : returnsOut[fact.to]) {
            if (graph.edge(edges[call].edge).callSite !=
                graph.edge(edges[ret].edge).callSite) continue;
            insert({edges[call].from, edges[ret].to, true,
                    ProofKind::Match, call, id, ret});
          }
      }
    } catch (const Budget &budget) {
      limited = true;
      result.completion.stop(budget.reason, budget.limit, budget.observed);
    }
  }
  result.summaryPairs = facts.size();

  // Realizable paths have unmatched returns before unmatched calls. The final
  // bit records a nonempty path even when a product node is revisited.
  if (products.size() > std::numeric_limits<std::size_t>::max() / 4)
    throw std::length_error("UFG: quotient state space overflow");
  std::vector<bool> seen(products.size() * 4);
  std::vector<Parent> parent(seen.size());
  std::deque<ID> queue;
  auto encode = [&](ID productID, unsigned phase, bool positive) {
    return checked(std::size_t(productID) * 4 + std::size_t(phase) * 2 +
                   unsigned(positive));
  };
  auto enqueue = [&](ID state, ID previous, bool byFact, ID item) {
    if (seen[state]) return;
    seen[state] = true;
    parent[state] = {previous, byFact, item};
    queue.push_back(state);
  };
  std::map<FlowNodeID, ID> accepted;
  if (!seen.empty()) {
    for (ID root : roots) enqueue(encode(root, 0, false), InvalidID, false, InvalidID);
    while (!queue.empty()) {
      ID current = queue.front(); queue.pop_front();
      ID point = current / 4;
      unsigned phase = (current % 4) / 2;
      bool positive = current % 2;
      Product p = products[point];
      if (sinks.count(p.node) && accepts[p.state] && (!query.requireNonEmpty || positive))
        accepted.emplace(expanded ? Graph.originalNode(p.node) : p.node, current);
      auto push = [&](ID target, unsigned nextPhase, bool nonempty,
                      bool byFact, ID item) {
        enqueue(encode(target, nextPhase, positive || nonempty),
                current, byFact, item);
      };
      if (contextual) {
        for (ID id : factsOut[point]) {
          const Fact &fact = facts[id];
          push(fact.to, phase, fact.positive, true, id);
        }
      }
      for (ID id : outgoing[point]) {
        const ProductEdge &edge = edges[id];
        FlowKind kind = graph.edge(edge.edge).kind;
        if (!contextual || (kind != FlowKind::Call && kind != FlowKind::Return))
          push(edge.to, phase, true, false, id);
        else if (query.context == ContextMode::Realizable) {
          if (kind == FlowKind::Call) push(edge.to, 1, true, false, id);
          else if (kind == FlowKind::Return && phase == 0)
            push(edge.to, 0, true, false, id);
        }
      }
    }
  }

  result.completion.modelComplete = !incomplete;
  result.exhaustive = result.completion.complete();
  result.status = !accepted.empty() ? QueryStatus::Found :
                  result.exhaustive ? QueryStatus::NotFound : QueryStatus::Unknown;
  result.message = limited ? "UFG tabulation budget exhausted" :
                   incomplete ? "incomplete model or omitted thread edges" :
                                "UFG lane tabulation complete";

  for (const auto &hit : accepted) {
    QueryResult witness;
    witness.completion = result.completion;
    witness.status = QueryStatus::Found;
    witness.productStates = result.productStates;
    witness.edgesExamined = result.edgesExamined;
    witness.summaryPairs = result.summaryPairs;
    witness.message = "witness in the supplied UFG abstraction; feasibility not proven";
    std::vector<Parent> steps;
    ID current = hit.second;
    while (parent[current].previous != InvalidID) {
      steps.push_back(parent[current]);
      current = parent[current].previous;
    }
    std::reverse(steps.begin(), steps.end());
    const Product &root = products[current / 4];
    witness.nodes.push_back(root.node);
    witness.automatonStates.push_back(root.state);
    auto append = [&](ID id) {
      if (query.maxWitnessEdges && witness.edges.size() >= query.maxWitnessEdges)
        throw WitnessBudget{};
      const ProductEdge &edge = edges[id];
      witness.edges.push_back(edge.edge);
      witness.nodes.push_back(products[edge.to].node);
      witness.automatonStates.push_back(products[edge.to].state);
    };
    try {
      for (const Parent &step : steps) {
        if (!step.fact) { append(step.item); continue; }
        std::vector<std::pair<bool, ID>> pending = {{true, step.item}};
        while (!pending.empty()) {
          auto item = pending.back(); pending.pop_back();
          if (!item.first) { append(item.second); continue; }
          const Fact &fact = facts[item.second];
          switch (fact.kind) {
          case ProofKind::Empty: break;
          case ProofKind::Edge: pending.push_back({false, fact.first}); break;
          case ProofKind::Concat:
            pending.push_back({true, fact.second});
            pending.push_back({true, fact.first});
            break;
          case ProofKind::Match:
            pending.push_back({false, fact.third});
            pending.push_back({true, fact.second});
            pending.push_back({false, fact.first});
            break;
          }
        }
      }
    } catch (const WitnessBudget &) {
      witness.witnessComplete = false;
      witness.message += "; witness rendering truncated";
    }
    result.foundAt.emplace(hit.first, std::move(witness));
  }
  return result;
}

QueryResult SearchEngine::run(Query query, ObjectID object) const {
  auto result = scan(std::move(query), object);
  if (!result.foundAt.empty()) return std::move(result.foundAt.begin()->second);
  QueryResult answer;
  answer.completion = result.completion;
  answer.status = result.status;
  answer.productStates = result.productStates;
  answer.edgesExamined = result.edgesExamined;
  answer.summaryPairs = result.summaryPairs;
  answer.message = std::move(result.message);
  return answer;
}

QueryResult SearchEngine::runGeneric(Query query) const {
  auto result = scanGeneric(std::move(query));
  if (!result.foundAt.empty()) return std::move(result.foundAt.begin()->second);
  QueryResult answer;
  answer.completion = result.completion;
  answer.status = result.status;
  answer.productStates = result.productStates;
  answer.edgesExamined = result.edgesExamined;
  answer.summaryPairs = result.summaryPairs;
  answer.message = std::move(result.message);
  return answer;
}

} // namespace ufg
} // namespace lotus
