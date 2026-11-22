#include "CFL/ConditionalDyck/ConditionalDyck.h"

#include "CFL/ConditionalDyck/ConCRA.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace lotus::cfl::conditional_dyck {
namespace {

unsigned roleBit(Boundary role) {
  switch (role) {
  case Boundary::Input:
    return 1;
  case Boundary::Output:
    return 2;
  case Boundary::Call:
    return 4;
  case Boundary::Return:
    return 8;
  }
  throw std::invalid_argument("invalid conditional-Dyck boundary role");
}

void validateLabel(Label label) {
  switch (label.kind) {
  case EdgeKind::Neutral:
    if (label.call_site != 0)
      throw std::invalid_argument("neutral edges must have call-site ID zero");
    return;
  case EdgeKind::Open:
  case EdgeKind::Close:
    return;
  }
  throw std::invalid_argument("invalid conditional-Dyck edge label");
}

std::size_t languageIndex(Language language) {
  switch (language) {
  case Language::Balanced:
    return 0;
  case Language::Left:
    return 1;
  case Language::Right:
    return 2;
  case Language::Extended:
    return 3;
  }
  throw std::invalid_argument("invalid conditional-Dyck query language");
}

enum class Scope { Library, Client, LibraryCall, Callback };
void registerScope(std::map<CallSite, Scope> &scopes, Label label,
                   Scope scope) {
  if (label.kind == EdgeKind::Neutral)
    return;
  const auto inserted = scopes.emplace(label.call_site, scope);
  if (!inserted.second && inserted.first->second != scope)
    throw std::invalid_argument("call-site ID reused across library/client "
                                "boundary scopes");
}

} // namespace

bool Label::operator==(const Label &other) const {
  return kind == other.kind && call_site == other.call_site;
}
bool Label::operator<(const Label &other) const {
  return std::tie(kind, call_site) < std::tie(other.kind, other.call_site);
}
bool Edge::operator==(const Edge &other) const {
  return source == other.source && target == other.target &&
         label == other.label;
}
bool Edge::operator<(const Edge &other) const {
  return std::tie(source, target, label) <
         std::tie(other.source, other.target, other.label);
}
void Graph::addNode(Node node) { nodes_.insert(node); }
bool Graph::addEdge(Node source, Node target, Label label) {
  validateLabel(label);
  addNode(source);
  addNode(target);
  const Edge edge{source, target, label};
  if (!edge_set_.insert(edge).second)
    return false;
  edges_.push_back(edge);
  return true;
}
void Library::markBoundary(Node node, Boundary role) {
  const unsigned bit = roleBit(role);
  graph_.addNode(node);
  boundaries_[node] |= bit;
}
void Library::addConditionalEdge(Node source, Node target, Label label,
                                 Condition condition) {
  validateLabel(label);
  graph_.addNode(source);
  graph_.addNode(target);
  conditional_.push_back({{source, target, label}, condition});
}
void Library::addCallback(Node source, Node target) {
  markBoundary(source, Boundary::Call);
  markBoundary(target, Boundary::Return);
  callbacks_.emplace(source, target);
}

struct Summary::Impl {
  Options options;
  SummaryStatistics statistics;
  std::set<Node> nodes;
  std::map<Node, unsigned> boundaries;
  std::set<std::pair<Node, Node>> callbacks;
  std::set<Condition> conditions;
  std::map<CallSite, Scope> scopes;
  std::vector<detail::ConditionalFact> rules;
};

Summary::Summary(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
SummaryStatistics Summary::statistics() const {
  if (!impl_)
    throw std::invalid_argument("moved-from conditional-Dyck summary");
  return impl_->statistics;
}
Options Summary::options() const {
  if (!impl_)
    throw std::invalid_argument("moved-from conditional-Dyck summary");
  return impl_->options;
}
bool Reachability::contains(Node source, Node target, Language language) const {
  return pairs(language).count({source, target}) != 0;
}
const std::set<std::pair<Node, Node>> &
Reachability::pairs(Language language) const {
  return pairs_[languageIndex(language)];
}

Summary Solver::summarize(const Library &library, Options options) const {
  if (options.max_premises && *options.max_premises == 0)
    throw std::invalid_argument(
        "ConCRA-k requires k >= 1; use nullopt for full");
  auto result = std::make_shared<Summary::Impl>();
  result->options = options;
  result->nodes = library.graph_.nodes();
  result->boundaries = library.boundaries_;
  result->callbacks = library.callbacks_;

  detail::Saturation engine(options.max_premises);
  engine.seedNodes(result->nodes);
  for (const auto &edge : library.graph_.edges()) {
    registerScope(result->scopes, edge.label, Scope::Library);
    engine.seed(detail::terminal(edge));
  }
  for (const auto &input : library.conditional_) {
    registerScope(result->scopes, input.edge.label, Scope::Library);
    result->conditions.insert(input.condition);
    engine.seed(detail::terminal(input.edge),
                {detail::Premise::guard(input.condition)});
  }
  for (const auto &callback : library.callbacks_) {
    const detail::Fact fact{callback.first, callback.second, detail::Symbol::S,
                            0};
    engine.seed(fact, {detail::Premise::reach(fact)});
  }
  engine.run();
  result->rules = detail::extract(engine, result->boundaries);
  result->statistics.conditional_facts = engine.facts().size();
  result->statistics.summary_facts = result->rules.size();
  result->statistics.bridging_facts = engine.bridges().size();
  for (const auto &fact : engine.facts())
    result->statistics.largest_premise_set =
        std::max(result->statistics.largest_premise_set, fact.premises.size());
  return Summary(std::move(result));
}

Reachability Solver::analyze(const Summary &summary, const Graph &client,
                             const std::set<Condition> &satisfied) const {
  if (!summary.impl_)
    throw std::invalid_argument("moved-from conditional-Dyck summary");
  const auto &data = *summary.impl_;
  for (Condition condition : satisfied)
    if (!data.conditions.count(condition))
      throw std::invalid_argument("undeclared conditional-library condition");

  auto scopes = data.scopes;
  std::map<CallSite, std::set<Node>> callback_calls, callback_returns;
  auto hasRole = [&](Node node, Boundary role) {
    const auto found = data.boundaries.find(node);
    return found != data.boundaries.end() && (found->second & roleBit(role));
  };
  for (const auto &edge : client.edges()) {
    const bool source_library = data.nodes.count(edge.source) != 0;
    const bool target_library = data.nodes.count(edge.target) != 0;
    Scope scope = Scope::Client;
    if (source_library && target_library)
      throw std::invalid_argument(
          "client must not supply library-internal edges");
    if (source_library) {
      if (edge.label.kind == EdgeKind::Open &&
          hasRole(edge.source, Boundary::Call)) {
        scope = Scope::Callback;
        callback_calls[edge.label.call_site].insert(edge.source);
      } else if (edge.label.kind == EdgeKind::Close &&
                 hasRole(edge.source, Boundary::Output)) {
        scope = Scope::LibraryCall;
      } else {
        throw std::invalid_argument("invalid outgoing library boundary edge");
      }
    } else if (target_library) {
      if (edge.label.kind == EdgeKind::Open &&
          hasRole(edge.target, Boundary::Input)) {
        scope = Scope::LibraryCall;
      } else if (edge.label.kind == EdgeKind::Close &&
                 hasRole(edge.target, Boundary::Return)) {
        scope = Scope::Callback;
        callback_returns[edge.label.call_site].insert(edge.target);
      } else {
        throw std::invalid_argument("invalid incoming library boundary edge");
      }
    }
    registerScope(scopes, edge.label, scope);
  }
  for (const auto &calls : callback_calls) {
    const auto returns = callback_returns.find(calls.first);
    if (returns == callback_returns.end())
      continue;
    for (Node source : calls.second)
      for (Node target : returns->second)
        if (!data.callbacks.count({source, target}))
          throw std::invalid_argument(
              "client callback has no declared premise");
  }

  detail::Saturation engine;
  engine.seedNodes(data.nodes);
  engine.seedNodes(client.nodes());
  for (const auto &edge : client.edges())
    engine.seed(detail::terminal(edge));

  // Algorithm 2: index each conditional summary by its still-missing facts.
  // Conditions are conjunctive; alternative rules with the same conclusion
  // remain separate. Cycles cannot justify themselves without an actual seed.
  std::vector<std::size_t> remaining(data.rules.size());
  std::map<detail::Fact, std::vector<std::size_t>> watchers;
  for (std::size_t id = 0; id < data.rules.size(); ++id) {
    const auto &rule = data.rules[id];
    bool enabled = true;
    for (const auto &premise : rule.premises)
      if (premise.kind == detail::Premise::Kind::Guard &&
          !satisfied.count(premise.condition))
        enabled = false;
    if (!enabled)
      continue;
    for (const auto &premise : rule.premises) {
      if (premise.kind == detail::Premise::Kind::Reach) {
        watchers[premise.fact].push_back(id);
        ++remaining[id];
      }
    }
    if (remaining[id] == 0)
      engine.seed(rule.fact);
  }
  engine.run([&](const detail::Fact &fact) {
    const auto found = watchers.find(fact);
    if (found == watchers.end())
      return;
    for (std::size_t id : found->second)
      if (--remaining[id] == 0)
        engine.seed(data.rules[id].fact);
  });

  Reachability result;
  for (const auto &derived : engine.facts()) {
    const auto &fact = derived.fact;
    const auto lang = detail::language(fact.symbol);
    if (lang && !data.nodes.count(fact.source) &&
        !data.nodes.count(fact.target))
      result.pairs_[languageIndex(*lang)].emplace(fact.source, fact.target);
  }
  return result;
}

Reachability Solver::solve(const Graph &graph) const {
  detail::Saturation engine;
  engine.seedNodes(graph.nodes());
  for (const auto &edge : graph.edges())
    engine.seed(detail::terminal(edge));
  engine.run();
  Reachability result;
  for (const auto &derived : engine.facts()) {
    const auto lang = detail::language(derived.fact.symbol);
    if (lang)
      result.pairs_[languageIndex(*lang)].emplace(derived.fact.source,
                                                  derived.fact.target);
  }
  return result;
}

} // namespace lotus::cfl::conditional_dyck
