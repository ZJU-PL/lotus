#include "IR/UseTraceSSA/Query.h"
#include "IR/UseTraceSSA/QueryContext.h"
#include "IR/UseTraceSSA/QueryMask.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace lotus {
namespace usetracessa {
ObjectUniverse::ObjectUniverse(std::vector<ObjectID> objects) : Objects(std::move(objects)) {
  std::sort(Objects.begin(), Objects.end());
  Objects.erase(std::unique(Objects.begin(), Objects.end()), Objects.end());
  if (Objects.size() >= InvalidID)
    throw std::length_error("UseTraceSSA: object universe too large");
}
std::size_t ObjectUniverse::index(ObjectID object) const {
  auto it = std::lower_bound(Objects.begin(), Objects.end(), object);
  if (it == Objects.end() || *it != object)
    throw std::out_of_range("UseTraceSSA: object outside query universe");
  return it - Objects.begin();
}
ObjectMask ObjectUniverse::mask(const ObjectSet &set) const {
  if (set.isUnknown()) return all();
  auto result = none();
  for (auto object : set.objects()) {
    auto it = std::lower_bound(Objects.begin(), Objects.end(), object);
    if (it != Objects.end() && *it == object) result.set(it - Objects.begin());
  }
  return result;
}
QueryStatus ObjectBatchResult::status(ObjectID object) const {
  auto bit = universe.index(object);
  return found.test(bit) ? QueryStatus::Found :
         unknown.test(bit) ? QueryStatus::Unknown : QueryStatus::NotFound;
}
namespace {
using Budget = detail::SearchBudgetExceeded;
// Query facts share immutable labels. TOP, BOTTOM, and unchanged masks need no
// new dense allocation; joins replace their handle instead of mutating aliases.
using MaskBits = detail::AdaptiveMask;
using Mask = std::shared_ptr<const MaskBits>;
ObjectMask exportMask(const MaskBits &bits) { return bits.bitVector(); }
std::uint64_t key(ID a, ID b) { return (std::uint64_t(a) << 32) | b; }
struct ProductNode { FlowNodeID node; ID state; };
struct ProductEdge { ID from, to; FlowEdgeID original; ID transition; };
struct ProductTransition {
  FlowNodeID node;
  ID state;
  FlowEdgeID original;
  Mask objects;
  ID edge = InvalidID;
};
struct EffectClass { Mask objects; Event events; Certainty certainty; };
struct Delta { ID id; Mask objects; bool positive = false; ID proof = InvalidID; };
struct Summary { ID from, to; Mask reachable, positive; };
} // namespace

struct ObjectWitnessData {
  static constexpr ID Choice = ID(1) << 31;
  struct Proof { ID parent; FlowEdgeID edge; FlowNodeID node; ID state; };
  struct ChoiceProof {
    ID before, after;
    Mask afterObjects;
  };
  struct Acceptance { Mask objects; ID proof; };
  std::vector<Proof> proofs;
  std::vector<ChoiceProof> choices;
  std::map<FlowNodeID, std::vector<Acceptance>> accepted;
  std::size_t maxEdges = 0;
};

QueryResult ObjectBatchResult::witness(FlowNodeID sink, ObjectID object) const {
  QueryResult result;
  result.completion = completion;
  result.status = QueryStatus::Unknown;
  result.message = "object witness provenance was not retained";
  if (!Witnesses) return result;
  auto bit = universe.index(object);
  auto accepted = Witnesses->accepted.find(sink);
  ID proof = InvalidID;
  if (accepted != Witnesses->accepted.end())
    for (const auto &candidate : accepted->second)
      if (candidate.objects->test(bit)) { proof = candidate.proof; break; }
  if (proof == InvalidID) {
    result.status = complete ? QueryStatus::NotFound : QueryStatus::Unknown;
    result.message = "no retained witness for this sink and object";
    return result;
  }
  result.status = QueryStatus::Found;
  result.message = "witness from symbolic object tabulation";
  result.productStates = statistics.productStates;
  result.edgesExamined = statistics.edgesExamined;
  result.summaryPairs = statistics.summaryPairs;
  std::vector<ID> path;
  for (ID id = proof; id != InvalidID;) {
    if (id & ObjectWitnessData::Choice) {
      const auto &choice = Witnesses->choices[id & ~ObjectWitnessData::Choice];
      id = choice.afterObjects->test(bit) ? choice.after : choice.before;
    } else {
      path.push_back(id);
      id = Witnesses->proofs[id].parent;
    }
  }
  std::reverse(path.begin(), path.end());
  for (ID id : path) {
    const auto &step = Witnesses->proofs[id];
    if (step.parent != InvalidID) {
      if (Witnesses->maxEdges && result.edges.size() >= Witnesses->maxEdges) {
        result.witnessComplete = false;
        result.message += "; witness rendering truncated";
        break;
      }
      result.edges.push_back(step.edge);
    }
    result.nodes.push_back(step.node);
    result.automatonStates.push_back(step.state);
  }
  return result;
}

ObjectBatchResult QueryEngine::runObjects(const ObjectBatchQuery &request) const {
  const auto &q = request.query;
  const auto &u = request.universe;
  if (q.memoryObject)
    throw std::invalid_argument("UseTraceSSA: batch query cannot specify a fixed object");
  if (!q.automaton.states || q.automaton.initial >= q.automaton.states)
    throw std::invalid_argument("UseTraceSSA: invalid automaton initial state");
  ObjectBatchResult result;
  result.universe = u;
  result.found = result.notFound = result.unknown = u.none();
  auto &stats = result.statistics;
  stats.candidateObjects = u.objects().size();
  Mask emptyMask = std::make_shared<MaskBits>(u.objects().size());
  Mask allMask =
      u.objects().empty()
          ? emptyMask
          : std::make_shared<MaskBits>(u.objects().size(), true);
  std::unordered_map<uintptr_t, Mask> smallMasks;
  auto makeMask = [&](MaskBits bits) -> Mask {
    if (bits.none())
      return emptyMask;
    if (bits.all())
      return allMask;
    bits.normalize();
    if (bits.isSmall()) {
      uintptr_t storage;
      auto word = bits.getData(storage)[0];
      auto cached = smallMasks.find(word);
      if (cached != smallMasks.end())
        return cached->second;
      auto mask = std::make_shared<MaskBits>(std::move(bits));
      // Bound interning memory even for adversarial combinations of guards.
      if (smallMasks.size() < 4096)
        smallMasks.emplace(word, mask);
      return mask;
    }
    return std::make_shared<MaskBits>(std::move(bits));
  };
  std::unordered_map<const std::vector<ObjectID> *, Mask> guardMasks;
  auto objectMask = [&](const ObjectSet &objects) -> Mask {
    if (objects.isUnknown()) return allMask;
    if (objects.empty()) return emptyMask;
    auto cached = guardMasks.find(&objects.objects());
    if (cached != guardMasks.end())
      return cached->second;
    MaskBits bits = u.objects().size() > 256 &&
                            objects.objects().size() <= u.objects().size() / 128
                        ? MaskBits::sparse(u.objects().size())
                        : MaskBits(u.objects().size());
    for (auto object : objects.objects()) {
      auto it =
          std::lower_bound(u.objects().begin(), u.objects().end(), object);
      if (it != u.objects().end() && *it == object)
        bits.set(it - u.objects().begin());
    }
    auto mask = makeMask(std::move(bits));
    guardMasks.emplace(&objects.objects(), mask);
    return mask;
  };
  auto intersect = [&](const Mask &a, const Mask &b) -> Mask {
    ++stats.maskIntersections;
    if (a == b || b == allMask || a == emptyMask) return a;
    if (a == allMask || b == emptyMask) return b;
    if (!a->anyCommon(*b))
      return emptyMask;
    MaskBits bits = *a;
    bits &= *b;
    if (bits == *a) return a;
    if (bits == *b) return b;
    return makeMask(std::move(bits));
  };
  auto unite = [&](Mask &a, const Mask &b) {
    ++stats.maskUnions;
    if (a == b || a == allMask || b == emptyMask) return;
    if (a == emptyMask || b == allMask) { a = b; return; }
    if (!b->test(*a))
      return;
    MaskBits bits = *a;
    bits |= *b;
    if (bits == *a) return;
    if (bits == *b) { a = b; return; }
    a = makeMask(std::move(bits));
  };
  auto difference = [&](const Mask &a, const Mask &b) -> Mask {
    if (a == b || b == allMask) return emptyMask;
    if (b == emptyMask || a == emptyMask) return a;
    if (a == allMask)
      return makeMask(b->complemented());
    if (!a->test(*b))
      return emptyMask;
    MaskBits bits = *a;
    bits.reset(*b);
    if (bits == *a) return a;
    return makeMask(std::move(bits));
  };
  Mask foundObjects = emptyMask;
  std::map<FlowNodeID, Mask> foundAt;
  auto bound = detail::checkSearchBudget;
  std::vector<bool> accepts(q.automaton.states), sinks(G.nodes().size()), traps(G.nodes().size());
  for (auto state : q.automaton.accepting) {
    if (state >= accepts.size()) throw std::invalid_argument("UseTraceSSA: accepting state");
    accepts[state] = true;
  }
  for (auto n : q.sources) G.node(n);
  for (auto n : q.sinks) { G.node(n); sinks[n] = true; }
  for (auto n : q.traps) { G.node(n); traps[n] = true; }

  // Cached only for this invocation: graph revision and universe are immutable
  // for its lifetime. Split by effect guards, never by individual object IDs.
  std::map<FlowNodeID, std::vector<EffectClass>> partitions;
  auto partition = [&](FlowNodeID id) -> const std::vector<EffectClass> & {
    auto found = partitions.find(id);
    if (found != partitions.end()) return found->second;
    const auto &n = G.node(id);
    std::vector<EffectClass> classes;
    if (!traps[id] && !u.objects().empty()) classes.push_back({allMask, n.events, n.certainty});
    for (const auto &effect : n.effects) {
      Mask guard = objectMask(effect.objects);
      std::vector<EffectClass> next;
      for (const auto &c : classes) {
        auto yes = intersect(c.objects, guard), no = difference(c.objects, guard);
        if (no != emptyMask) next.push_back({std::move(no), c.events, c.certainty});
        if (yes != emptyMask) next.push_back({std::move(yes), c.events | effect.events,
            c.certainty == Certainty::May || effect.certainty == Certainty::May ||
            effect.objects.isUnknown() ? Certainty::May : Certainty::Must});
      }
      // Equal semantics may be reunited; distinct effects remain intact in IR.
      std::map<std::pair<Event, Certainty>, Mask> merged;
      for (const auto &c : next) {
        auto it = merged.emplace(std::make_pair(c.events, c.certainty), emptyMask).first;
        unite(it->second, c.objects);
      }
      classes.clear();
      for (auto &c : merged)
        classes.push_back({std::move(c.second), c.first.first, c.first.second});
    }
    return partitions.emplace(id, std::move(classes)).first->second;
  };
  std::unordered_map<detail::FlowStateKey, std::map<ID, Mask>, detail::FlowStateHash> effectCache;
  auto effects = [&](FlowNodeID node, ID state) -> const std::map<ID, Mask> & {
    detail::FlowStateKey k{node, state};
    auto cached = effectCache.find(k);
    if (cached != effectCache.end()) return cached->second;
    std::map<ID, Mask> transitions;
    auto add = [&](ID next, const Mask &objects) {
      if (next >= q.automaton.states)
        throw std::invalid_argument("UseTraceSSA: automaton transition out of range");
      auto it = transitions.emplace(next, emptyMask).first;
      unite(it->second, objects);
    };
    for (const auto &c : partition(node)) {
      if (!hasEvent(c.events, q.trapEvents))
        add(q.automaton.transition ? q.automaton.transition(state, c.events) : state, c.objects);
      if (c.certainty == Certainty::May) add(state, c.objects);
    }
    return effectCache.emplace(k, std::move(transitions)).first->second;
  };

  // Both modes use precisely (FlowNodeID, AutomatonState). Masks are payloads,
  // not a third product dimension. Incoming deltas intersect transition labels.
  std::vector<ProductNode> pn;
  std::vector<ProductEdge> pe;
  std::vector<std::vector<ID>> pout;
  // Transition labels are compiled once and shared with materialized edges.
  // A target product is created only when a reachable delta uses its label.
  std::vector<std::vector<ProductTransition>> transitions;
  std::vector<Mask> omittedThreads;
  std::vector<Mask> reached;
  std::vector<bool> expanded;
  std::vector<Mask> edgeMasks(G.edges().size());
  std::vector<bool> edgeMaskReady(G.edges().size());
  std::unordered_map<detail::FlowStateKey, ID, detail::FlowStateHash> products;
  std::map<ID, Mask> roots;
  std::deque<Delta> pending;
  const bool boundedContext =
      q.contextLimit && q.context != ContextMode::Insensitive;
  const bool directTraversal =
      boundedContext || q.context == ContextMode::Insensitive;
  Mask incomplete = G.complete() ? emptyMask : allMask;
  bool limited = false, hasContext = false;
  auto product = [&](FlowNodeID node, ID state) -> ID {
    detail::FlowStateKey k{node, state};
    auto it = products.find(k);
    if (it != products.end()) return it->second;
    bound(pn.size(), q.maxProductStates, SearchStopReason::ProductStates);
    if (pn.size() >= InvalidID / 4) throw std::length_error("UseTraceSSA: product too large");
    ID id = pn.size();
    products.emplace(k, id); pn.push_back({node, state});
    pout.emplace_back(); transitions.emplace_back(); omittedThreads.push_back(emptyMask);
    reached.push_back(emptyMask); expanded.push_back(false);
    return id;
  };
  auto reach = [&](ID id, const Mask &objects) {
    auto delta = difference(objects, reached[id]);
    if (delta == emptyMask) return;
    unite(reached[id], delta); ++stats.nonemptyDeltas;
    pending.push_back({id, std::move(delta)});
  };
  auto productMask = [&](const ProductEdge &edge) -> const Mask & {
    return transitions[edge.from][edge.transition].objects;
  };
  auto expand = [&](ID id) {
    if (expanded[id])
      return;
    expanded[id] = true;
    ProductNode n = pn[id];
    for (auto eid : G.outgoing(n.node)) {
      ++stats.edgesExamined;
      const auto &e = G.edge(eid);
      if (!e.enabled || e.objects.empty() || (q.edgeFilter && !q.edgeFilter(e)))
        continue;
      if (!edgeMaskReady[eid]) {
        edgeMasks[eid] = objectMask(e.objects);
        edgeMaskReady[eid] = true;
      }
      const auto &edgeMask = edgeMasks[eid];
      if (e.kind == FlowKind::Thread && !q.includeThreadEdges) {
        unite(omittedThreads[id], edgeMask);
        continue;
      }
      for (const auto &t : effects(e.to, n.state)) {
        auto label = intersect(edgeMask, t.second);
        if (label != emptyMask)
          transitions[id].push_back({e.to, t.first, eid, std::move(label)});
      }
    }
  };
  auto materialize = [&](ID from, std::size_t index) -> ID {
    ID id = transitions[from][index].edge;
    if (id != InvalidID)
      return id;
    // product() can grow the transition vector; copy the fields first.
    const auto node = transitions[from][index].node;
    const auto state = transitions[from][index].state;
    const auto original = transitions[from][index].original;
    ID target = product(node, state);
    id = pe.size();
    pe.push_back({from, target, original, static_cast<ID>(index)});
    transitions[from][index].edge = id;
    pout[from].push_back(id);
    const auto kind = G.edge(original).kind;
    hasContext |= kind == FlowKind::Call || kind == FlowKind::Return;
    return id;
  };
  try {
    for (auto source : q.sources) for (const auto &t : effects(source, q.automaton.initial)) {
      auto p = product(source, t.first);
      unite(roots.emplace(p, emptyMask).first->second, t.second);
      if (!directTraversal)
        reach(p, t.second);
    }
    while (!pending.empty()) {
      Delta delta = std::move(pending.front()); pending.pop_front();
      expand(delta.id);
      if (omittedThreads[delta.id] != emptyMask)
        unite(incomplete, intersect(delta.objects, omittedThreads[delta.id]));
      for (std::size_t i = 0; i < transitions[delta.id].size(); ++i) {
        const auto &transition = transitions[delta.id][i];
        auto arriving = intersect(delta.objects, transition.objects);
        if (arriving == emptyMask) continue;
        ID id = materialize(delta.id, i);
        reach(pe[id].to, arriving);
      }
    }
  } catch (const Budget &budget) {
    limited = true;
    result.completion.stop(budget.reason, budget.limit, budget.observed);
  }
  stats.productStates = pn.size(); stats.productEdges = pe.size();
  const bool context = hasContext && q.context != ContextMode::Insensitive;
  if (request.retainWitnesses && (q.contextLimit || !context)) {
    result.Witnesses = std::make_shared<ObjectWitnessData>();
    result.Witnesses->maxEdges = q.maxWitnessEdges;
  }
  auto proof = [&](ID parent, FlowEdgeID edge, ID product) -> ID {
    if (!result.Witnesses) return InvalidID;
    auto &proofs = result.Witnesses->proofs;
    if (proofs.size() >= ObjectWitnessData::Choice)
      throw std::length_error("UseTraceSSA: witness provenance too large");
    ID id = proofs.size();
    proofs.push_back({parent, edge, pn[product].node, pn[product].state});
    return id;
  };
  std::unordered_map<FlowNodeID, Mask> sinkMasks;
  auto accept = [&](FlowNodeID sink, const Delta &delta) {
    Mask objects = delta.objects;
    if (q.sinkEvents != Event::None) {
      auto cached = sinkMasks.find(sink);
      if (cached == sinkMasks.end()) {
        Mask relevant = emptyMask;
        for (const auto &effect : partition(sink))
          if (hasEvent(effect.events, q.sinkEvents))
            unite(relevant, effect.objects);
        cached = sinkMasks.emplace(sink, std::move(relevant)).first;
      }
      objects = intersect(objects, cached->second);
    }
    if (objects == emptyMask)
      return;
    auto &accepted = foundAt.emplace(sink, emptyMask).first->second;
    if (result.Witnesses) {
      // A delta's objects all follow its parent's proof. Only acceptance needs
      // a mask: the immutable parent chain is already compatible with every bit.
      auto fresh = difference(objects, accepted);
      if (fresh != emptyMask)
        result.Witnesses->accepted[sink].push_back({std::move(fresh), delta.proof});
    }
    unite(foundObjects, objects);
    unite(accepted, objects);
    if (request.stopAfterFirstFinding) {
      limited = true;
      result.completion.stop(SearchStopReason::FirstFinding);
    }
  };
  auto exportResult = [&] {
    auto unknown = difference(limited ? allMask : incomplete, foundObjects);
    result.found = exportMask(*foundObjects);
    result.unknown = exportMask(*unknown);
    result.notFound =
        exportMask(*difference(difference(allMask, foundObjects), unknown));
    result.completion.modelComplete = G.complete() && incomplete == emptyMask;
    result.complete = result.completion.complete();
    for (const auto &sink : foundAt) {
      auto bit = sink.second->find_first();
      result.sinks.push_back(
          {sink.first, u.objects()[bit], sink.second->count()});
      if (request.retainSinkMasks)
        result.foundAt.emplace(sink.first, exportMask(*sink.second));
    }
    stats.foundObjects = result.found.count();
    stats.notFoundObjects = result.notFound.count();
    stats.unknownObjects = result.unknown.count();
  };

  if (directTraversal) {
    using ContextState = detail::InternedContextState;
    detail::CallStrings calls(q.contextLimit.value_or(0));
    std::unordered_map<ContextState, ID, detail::InternedContextHash> known;
    std::vector<ContextState> states;
    std::vector<Mask> visited, queued;
    std::vector<ID> queuedProof;
    std::deque<ID> queue;
    auto push = [&](ContextState state, const Mask &objects,
                    ID parent = InvalidID, FlowEdgeID edge = InvalidFlowID) {
      if (objects == emptyMask) return;
      auto found = known.find(state);
      ID id;
      if (found == known.end()) {
        bound(states.size(), q.maxSummaryPairs, SearchStopReason::SummaryPairs);
        if (states.size() >= InvalidID)
          throw std::length_error("UseTraceSSA: context state identifier overflow");
        id = states.size();
        known.emplace(state, id);
        states.push_back(state);
        visited.push_back(emptyMask);
        queued.push_back(emptyMask);
        queuedProof.push_back(InvalidID);
      } else {
        id = found->second;
      }
      auto delta = difference(objects, visited[id]);
      if (delta == emptyMask) return;
      unite(visited[id], delta);
      ++stats.nonemptyDeltas;
      ID evidence = proof(parent, edge, states[id].product);
      if (queued[id] == emptyMask) {
        queue.push_back(id);
        queuedProof[id] = evidence;
      } else if (result.Witnesses) {
        auto &choices = result.Witnesses->choices;
        if (choices.size() >= ObjectWitnessData::Choice - 1)
          throw std::length_error(
              "UseTraceSSA: witness choice identifier overflow");
        ID choice = choices.size() | ObjectWitnessData::Choice;
        choices.push_back({queuedProof[id], evidence, delta});
        queuedProof[id] = choice;
      }
      unite(queued[id], delta);
    };
    try {
      for (const auto &root : roots)
        push({root.first, 0, false, false}, root.second);
      while (!queue.empty()) {
        ID id = queue.front();
        queue.pop_front();
        Delta delta{id, std::move(queued[id]), false, queuedProof[id]};
        queued[id] = emptyMask;
        queuedProof[id] = InvalidID;
        ContextState current = states[delta.id];
        ProductNode point = pn[current.product];
        if (sinks[point.node] && accepts[point.state] &&
            (!q.requireNonEmpty || current.positive) &&
            (q.context != ContextMode::Balanced ||
             calls.empty(current.stack))) {
          accept(point.node, delta);
          if (request.stopAfterFirstFinding && !foundAt.empty())
            break;
        }
        expand(current.product);
        if (omittedThreads[current.product] != emptyMask)
          unite(incomplete,
                intersect(delta.objects, omittedThreads[current.product]));
        for (std::size_t index = 0; index < transitions[current.product].size();
             ++index) {
          const auto original = transitions[current.product][index].original;
          auto permitted = intersect(
              delta.objects, transitions[current.product][index].objects);
          if (permitted == emptyMask) continue;
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
          push(next, permitted, delta.proof, original);
        }
      }
    } catch (const Budget &budget) {
      limited = true;
      result.completion.stop(budget.reason, budget.limit, budget.observed);
    }
    stats.summaryPairs = states.size();
    stats.productStates = pn.size();
    stats.productEdges = pe.size();
    exportResult();
    result.message =
        result.completion.stopReason == SearchStopReason::FirstFinding
            ? "first valid finding; sink enumeration stopped"
        : limited                 ? "bounded-context query budget exhausted"
        : incomplete != emptyMask ? "incomplete model or omitted thread edges"
                                  : "bounded-context mask tabulation complete";
    return result;
  }

  // D ::= epsilon | local | D D | call_c D return_c. Sequential composition
  // intersects; alternative derivations union for the SAME relation pair.
  // Separate positive masks retain nonempty cycles even when epsilon exists.
  std::vector<Summary> summaries;
  std::vector<std::vector<ID>> rout(pn.size());
  std::size_t bodyFacts = 0;
  if (context) {
    // Tabulate balanced reachability only from demanded callee entries. Local
    // steps and discovered matched calls propagate each entry's row. The
    // quotient needs matched-call edges, not an all-pairs transitive closure.
    std::vector<Summary> facts;
    std::vector<std::vector<ID>> factsAt(pn.size()), callsIn(pn.size());
    std::vector<std::unordered_map<CallSiteID, std::vector<ID>>> returnsBySite(pn.size());
    std::unordered_map<std::uint64_t, ID> rows, matched;
    std::deque<Delta> queue;
    auto insert = [&](ID entry, ID node, const Mask &objects) {
      if (objects == emptyMask)
        return;
      auto k = key(entry, node);
      auto found = rows.find(k);
      if (found == rows.end()) {
        bound(facts.size() + summaries.size(), q.maxSummaryPairs,
              SearchStopReason::SummaryPairs);
        ID id = facts.size();
        found = rows.emplace(k, id).first;
        facts.push_back({entry, node, emptyMask, emptyMask});
        factsAt[node].push_back(id);
      }
      auto &old = facts[found->second].reachable;
      auto delta = difference(objects, old);
      if (delta == emptyMask)
        return;
      unite(old, delta);
      ++stats.nonemptyDeltas;
      queue.push_back({found->second, std::move(delta)});
    };
    auto match = [&](ID from, ID to, const Mask &objects) {
      if (objects == emptyMask)
        return;
      auto k = key(from, to);
      auto found = matched.find(k);
      if (found == matched.end()) {
        bound(facts.size() + summaries.size(), q.maxSummaryPairs,
              SearchStopReason::SummaryPairs);
        ID id = summaries.size();
        found = matched.emplace(k, id).first;
        summaries.push_back({from, to, emptyMask, emptyMask});
        rout[from].push_back(id);
      }
      auto delta = difference(objects, summaries[found->second].reachable);
      if (delta == emptyMask)
        return;
      unite(summaries[found->second].reachable, delta);
      summaries[found->second].positive = summaries[found->second].reachable;
      const auto count = factsAt[from].size();
      for (std::size_t i = 0; i < count; ++i) {
        auto fact = facts[factsAt[from][i]];
        insert(fact.from, to, intersect(fact.reachable, delta));
      }
    };
    try {
      for (ID i = 0; i < pe.size(); ++i) {
        const auto &e = pe[i]; auto kind = G.edge(e.original).kind;
        if (kind == FlowKind::Call) {
          callsIn[e.to].push_back(i);
          insert(e.to, e.to, allMask);
        } else if (kind == FlowKind::Return)
          returnsBySite[e.from][G.edge(e.original).callSite].push_back(i);
      }
      while (!queue.empty()) {
        Delta d = std::move(queue.front()); queue.pop_front();
        ID entry = facts[d.id].from, node = facts[d.id].to;
        for (auto edgeID : pout[node]) {
          const auto &edge = pe[edgeID];
          auto kind = G.edge(edge.original).kind;
          if (kind != FlowKind::Call && kind != FlowKind::Return)
            insert(entry, edge.to, intersect(d.objects, productMask(edge)));
        }
        auto count = rout[node].size();
        for (std::size_t i = 0; i < count; ++i) {
          auto summary = summaries[rout[node][i]];
          insert(entry, summary.to, intersect(d.objects, summary.reachable));
        }
        for (auto call : callsIn[entry]) {
          auto site = G.edge(pe[call].original).callSite;
          auto returns = returnsBySite[node].find(site);
          if (returns == returnsBySite[node].end())
            continue;
          for (auto ret : returns->second)
            match(pe[call].from, pe[ret].to,
                  intersect(intersect(productMask(pe[call]), d.objects),
                            productMask(pe[ret])));
        }
      }
    } catch (const Budget &budget) {
      limited = true;
      result.completion.stop(budget.reason, budget.limit, budget.observed);
    }
    bodyFacts = facts.size();
  }
  stats.summaryPairs = bodyFacts + summaries.size();

  // Existing realizable quotient: unmatched returns precede unmatched calls.
  // Delta masks belong to logical (product, phase, positive) states; no object
  // creates a separate search state or a per-object witness recipe.
  std::vector<Mask> visited(pn.size() * 4, emptyMask);
  std::deque<Delta> queue;
  auto push = [&](ID state, const Mask &objects,
                  ID parent = InvalidID, FlowEdgeID edge = InvalidFlowID) {
    auto delta = difference(objects, visited[state]);
    if (delta == emptyMask) return;
    unite(visited[state], delta); ++stats.nonemptyDeltas;
    queue.push_back({state, std::move(delta), false, proof(parent, edge, state / 4)});
  };
  for (const auto &root : roots) push(root.first * 4, root.second);
  while (!queue.empty()) {
    Delta d = std::move(queue.front()); queue.pop_front();
    ID p = d.id / 4; unsigned phase = (d.id % 4) / 2; bool positive = d.id % 2;
    if (sinks[pn[p].node] && accepts[pn[p].state] && (!q.requireNonEmpty || positive)) {
      accept(pn[p].node, d);
      if (request.stopAfterFirstFinding && !foundAt.empty())
        break;
    }
    auto step = [&](ID target, unsigned nextPhase, bool nonempty,
                    const Mask &mask, FlowEdgeID edge = InvalidFlowID) {
      push(target * 4 + nextPhase * 2 +
               unsigned(q.requireNonEmpty && (positive || nonempty)),
           intersect(d.objects, mask), d.proof, edge);
    };
    if (context) for (auto id : rout[p]) {
      const auto &s = summaries[id];
      step(s.to, phase, true, s.reachable);
    }
    for (auto id : pout[p]) {
      const auto &e = pe[id]; auto kind = G.edge(e.original).kind;
      if (!context || (kind != FlowKind::Call && kind != FlowKind::Return))
        step(e.to, phase, true, productMask(e), e.original);
      else if (q.context == ContextMode::Realizable) {
        if (kind == FlowKind::Call) step(e.to, 1, true, productMask(e), e.original);
        else if (kind == FlowKind::Return && !phase)
          step(e.to, 0, true, productMask(e), e.original);
      }
    }
  }
  exportResult();
  result.message =
      result.completion.stopReason == SearchStopReason::FirstFinding
          ? "first valid finding; sink enumeration stopped"
      : limited ? "symbolic query budget exhausted; remaining objects unknown"
      : incomplete != emptyMask ? "incomplete model or omitted thread edges"
                                : "symbolic same-object reachability complete";
  return result;
}
} // namespace usetracessa
} // namespace lotus
