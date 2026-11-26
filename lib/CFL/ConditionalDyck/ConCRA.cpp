// Adapted from the ConCRA ESOP 2017 artifact's summarize_f.cpp,
// summarize_k.cpp, library_graph.cpp and application_graph.cpp.
// Original implementation: Hao Tang, Di Wang and collaborators.
#include "CFL/ConditionalDyck/ConCRA.h"

#include <algorithm>
#include <iterator>

namespace lotus::cfl::conditional_dyck::detail {

Fact terminal(const Edge &edge) {
  Symbol symbol = Symbol::Neutral;
  if (edge.label.kind == EdgeKind::Open)
    symbol = Symbol::Open;
  else if (edge.label.kind == EdgeKind::Close)
    symbol = Symbol::Close;
  return {edge.source, edge.target, symbol, edge.label.call_site};
}

std::optional<Language> language(Symbol symbol) {
  switch (symbol) {
  case Symbol::S:
    return Language::Balanced;
  case Symbol::L:
    return Language::Left;
  case Symbol::R:
    return Language::Right;
  case Symbol::D:
    return Language::Extended;
  default:
    return std::nullopt;
  }
}

bool Saturation::subsumed(const Fact &fact, const Premises &premises) const {
  const auto found = known_.find(fact);
  if (found == known_.end())
    return false;
  for (FactId id : found->second) {
    const auto &existing = facts_[id].premises;
    if (std::includes(premises.begin(), premises.end(), existing.begin(),
                      existing.end()))
      return true;
  }
  return false;
}

void Saturation::derive(Fact fact, Premises premises,
                        const std::vector<FactId> &dependencies) {
  if (subsumed(fact, premises))
    return;
  if (bound_ && premises.size() > *bound_) {
    // Save every alternative derivation, even when the singleton bridge was
    // already inserted. Dropping these alternatives loses completeness.
    auto &support = bridges_[fact];
    support.insert(dependencies.begin(), dependencies.end());
    premises = {Premise::reach(fact)};
    if (subsumed(fact, premises))
      return;
  }
  const FactId id = facts_.size();
  known_[fact].push_back(id);
  facts_.push_back({fact, std::move(premises)});
  work_.emplace(facts_.back().premises.size(), id);
}

void Saturation::seed(Fact fact, Premises premises) {
  std::sort(premises.begin(), premises.end());
  premises.erase(std::unique(premises.begin(), premises.end()), premises.end());
  derive(fact, std::move(premises), {});
}

void Saturation::seedNodes(const std::set<Node> &nodes) {
  for (Node node : nodes)
    seed({node, node, Symbol::S, 0});
}

void Saturation::join(FactId left, FactId right) {
  const auto &a = facts_[left];
  const auto &b = facts_[right];
  const Symbol x = a.fact.symbol, y = b.fact.symbol;
  Symbol result;
  CallSite site = 0;
  if (x == Symbol::S && y == Symbol::S)
    result = Symbol::S;
  else if (x == Symbol::L && y == Symbol::L)
    result = Symbol::L;
  else if (x == Symbol::R && y == Symbol::R)
    result = Symbol::R;
  else if ((x == Symbol::R && y == Symbol::D) ||
           (x == Symbol::D && y == Symbol::L))
    result = Symbol::D;
  else if (x == Symbol::S && y == Symbol::Close) {
    result = Symbol::Tail;
    site = b.fact.call_site;
  } else if (x == Symbol::Open && y == Symbol::Tail &&
             a.fact.call_site == b.fact.call_site)
    result = Symbol::S;
  else
    return;

  Premises merged;
  std::set_union(a.premises.begin(), a.premises.end(), b.premises.begin(),
                 b.premises.end(), std::back_inserter(merged));
  derive({a.fact.source, b.fact.target, result, site}, std::move(merged),
         {left, right});
}

void Saturation::run(const std::function<void(const Fact &)> &on_fact) {
  while (!work_.empty()) {
    const FactId id = work_.top().second;
    work_.pop();
    const auto &current = facts_[id]; // deque references survive insertions
    const Fact fact = current.fact;
    outgoing_[fact.source].push_back(id);
    incoming_[fact.target].push_back(id);

    auto unary = [&](Symbol symbol) {
      derive({fact.source, fact.target, symbol, 0}, current.premises, {id});
    };
    switch (fact.symbol) {
    case Symbol::Neutral:
      unary(Symbol::S);
      break;
    case Symbol::Open:
      unary(Symbol::L);
      break;
    case Symbol::Close:
      unary(Symbol::R);
      break;
    case Symbol::S:
      unary(Symbol::L);
      unary(Symbol::R);
      unary(Symbol::D);
      break;
    default:
      break;
    }
    // Only processed facts enter these adjacency lists; derive() only enqueues,
    // so appending facts cannot invalidate the iterators used by either join.
    for (FactId next : outgoing_[fact.target])
      join(id, next);
    for (FactId previous : incoming_[fact.source])
      join(previous, id);
    if (on_fact)
      on_fact(fact);
  }
}

std::vector<ConditionalFact>
extract(const Saturation &engine, const std::map<Node, unsigned> &boundaries) {
  const auto &facts = engine.facts();
  std::vector<FactId> pending;
  std::set<FactId> retained;
  auto retain = [&](FactId id) {
    if (retained.insert(id).second)
      pending.push_back(id);
  };
  for (FactId id = 0; id < facts.size(); ++id) {
    const auto &fact = facts[id].fact;
    if (language(fact.symbol) && boundaries.count(fact.source) &&
        boundaries.count(fact.target))
      retain(id);
  }
  // Algorithm 3, AddDependencies: include intermediate facts required to
  // discharge bridge premises, with a visited set for recursive dependencies.
  for (std::size_t i = 0; i < pending.size(); ++i) {
    for (const auto &premise : facts[pending[i]].premises) {
      if (premise.kind != Premise::Kind::Reach)
        continue;
      const auto bridge = engine.bridges().find(premise.fact);
      if (bridge != engine.bridges().end())
        for (FactId dependency : bridge->second)
          retain(dependency);
    }
  }
  std::vector<ConditionalFact> result;
  result.reserve(retained.size());
  for (FactId id : retained)
    result.push_back(facts[id]);
  return result;
}

} // namespace lotus::cfl::conditional_dyck::detail
