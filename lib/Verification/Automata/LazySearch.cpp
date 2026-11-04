#include "Verification/Automata/Detail/Summary.h"
#include "Verification/Automata/Detail/Workspace.h"

#include <algorithm>
#include <deque>

namespace lotus::verification::automata {
namespace {
// A weighted saturation agenda interleaves prefix discovery and balanced
// summaries. Only a reachable prefix can expand a state's internal/call edges.
// Returns are requested for actual call/summary joins, never all state pairs.
class LazySearch {
  using PathId = detail::PathId;
  static constexpr auto None = detail::NoPath;
  struct Call {
    State source, target;
    Letter letter;
    PathId path;
  };
  struct Node {
    bool expanded = false, bottomExpanded = false;
    PathId empty = None;
    PathId prefix[2] = {None, None};
    std::pmr::vector<std::size_t> outgoing, incoming;
    std::pmr::vector<Call> calls, callers;
    std::pmr::vector<PathId> pendingReturns;
    explicit Node(std::pmr::memory_resource *memory)
        : outgoing(memory), incoming(memory), calls(memory), callers(memory),
          pendingReturns(memory) {}
  };
  struct Summary {
    State source, target;
    PathId path, processed = None;
  };
  struct Task {
    std::uint64_t priority, serial;
    bool summary;
    std::size_t id;
    PathId path;
    bool operator>(const Task &other) const {
      return priority != other.priority ? priority > other.priority
                                        : serial > other.serial;
    }
  };
  Provider operand_;
  SearchOptions options_;
  Context context_;
  detail::Workspace workspace_;
  std::pmr::unordered_map<State, Node> nodes_;
  std::pmr::vector<detail::Path> paths_;
  std::pmr::vector<Summary> summaries_;
  std::pmr::unordered_map<std::uint64_t, std::size_t> index_;
  std::priority_queue<Task, std::pmr::vector<Task>, std::greater<Task>> agenda_;
  std::uint64_t serial_ = 0;

  static std::uint64_t key(State p, State q) {
    return (std::uint64_t(p) << 32) | q;
  }
  PathId save(detail::Path path) {
    context_->check();
    if (paths_.size() >= context_->maxWitnessNodes)
      throw ResourceLimit();
    paths_.push_back(path);
    return paths_.size() - 1;
  }
  std::uint64_t cost(const Transition &t) const {
    auto value = std::uint64_t(1);
    if (options_.strategy == SearchStrategy::LoopPenalizingAStar) {
      if (operand_->alphabet().kind(t.letter) == LetterKind::Call)
        value = detail::addCost(value, options_.callPenalty);
      if (std::find(options_.loopHeads.begin(), options_.loopHeads.end(),
                    t.target) != options_.loopHeads.end())
        value = detail::addCost(value, options_.loopPenalty);
    }
    if (options_.extraCost)
      value = detail::addCost(value, options_.extraCost(t));
    return value;
  }
  PathId edge(const Transition &t) {
    return save(
        {t.source, t.target, cost(t), false, true, None, None, t.letter, 1});
  }
  PathId concat(PathId left, PathId right) {
    auto l = paths_[left], r = paths_[right];
    if (l.target != r.source)
      throw std::logic_error("disconnected lazy NWA witness");
    if (!l.nonempty)
      return right;
    if (!r.nonempty)
      return left;
    if (l.length > std::numeric_limits<std::size_t>::max() - r.length)
      throw ResourceLimit();
    return save({l.source, r.target, detail::addCost(l.cost, r.cost), false,
                 true, left, right, Bottom, l.length + r.length});
  }
  void schedule(bool summary, std::size_t id, PathId path) {
    context_->check();
    auto priority = paths_[path].cost;
    if (options_.strategy == SearchStrategy::SummaryReachability)
      priority = serial_;
    else if (!summary &&
             options_.strategy == SearchStrategy::LoopPenalizingAStar &&
             options_.heuristic)
      priority = detail::addCost(
          priority, options_.heuristic(static_cast<State>(id / 2)));
    agenda_.push({priority, serial_++, summary, id, path});
  }
  Node &node(State q) {
    auto it = nodes_.find(q);
    if (it != nodes_.end())
      return it->second;
    context_->check();
    if (nodes_.size() >= context_->maxStates)
      throw ResourceLimit();
    auto &result = nodes_.emplace(q, Node{workspace_.resource()}).first->second;
    result.empty = save({q, q, 0, false, false});
    offerSummary(result.empty);
    return result;
  }
  bool improvesSummary(State p, State q, std::uint64_t value) const {
    auto it = index_.find(key(p, q));
    return it == index_.end() ||
           value < paths_[summaries_[it->second].path].cost;
  }
  void offerSummary(PathId path) {
    context_->check();
    auto p = paths_[path];
    auto found = index_.find(key(p.source, p.target));
    if (found == index_.end()) {
      if (summaries_.size() >= context_->maxPairs)
        throw ResourceLimit();
      auto id = summaries_.size();
      index_.emplace(key(p.source, p.target), id);
      summaries_.push_back({p.source, p.target, path, None});
      nodes_.at(p.source).outgoing.push_back(id);
      nodes_.at(p.target).incoming.push_back(id);
      schedule(true, id, path);
    } else if (p.cost < paths_[summaries_[found->second].path].cost) {
      summaries_[found->second].path = path;
      schedule(true, found->second, path);
    }
  }
  void combine(PathId left, PathId right) {
    context_->check();
    auto l = paths_[left], r = paths_[right];
    if (improvesSummary(l.source, r.target, detail::addCost(l.cost, r.cost)))
      offerSummary(concat(left, right));
  }
  void prefix(State q, bool above, PathId path) {
    context_->check();
    auto &current = node(q).prefix[above];
    if (current == None || paths_[path].cost < paths_[current].cost) {
      current = path;
      schedule(false, 2 * std::size_t(q) + above, path);
    }
  }
  void extend(State q, bool above, PathId left, PathId right) {
    context_->check();
    auto current = node(q).prefix[above];
    auto value = detail::addCost(paths_[left].cost, paths_[right].cost);
    if (current == None || value < paths_[current].cost)
      prefix(q, above, concat(left, right));
  }
  void wrap(const Call &call, PathId body) {
    context_->check();
    auto path = paths_[body];
    for (Letter r : operand_->enabledLettersView(
             path.target, LetterKind::Return, call.source)) {
      context_->check();
      for (State t : operand_->successorsView(path.target, r, call.source)) {
        context_->check();
        node(t);
        Transition ret{path.target, r, t, call.source};
        auto value = detail::addCost(
            detail::addCost(paths_[call.path].cost, path.cost), cost(ret));
        if (improvesSummary(call.source, t, value)) {
          auto returnPath = edge(ret);
          offerSummary(concat(concat(call.path, body), returnPath));
        }
      }
    }
  }
  void expand(State q) {
    auto &current = nodes_.at(q);
    if (current.expanded)
      return;
    current.expanded = true;
    for (Letter a : operand_->enabledLettersView(q, LetterKind::Internal)) {
      context_->check();
      for (State t : operand_->successorsView(q, a)) {
        context_->check();
        node(t);
        Transition transition{q, a, t};
        if (improvesSummary(q, t, cost(transition)))
          offerSummary(edge(transition));
      }
    }
    for (Letter a : operand_->enabledLettersView(q, LetterKind::Call)) {
      context_->check();
      for (State t : operand_->successorsView(q, a)) {
        context_->check();
        auto &callee = node(t);
        Call call{q, t, a, edge({q, a, t})};
        current.calls.push_back(call);
        callee.callers.push_back(call);
        auto count = callee.outgoing.size();
        for (std::size_t i = 0; i < count; ++i) {
          context_->check();
          auto s = summaries_[callee.outgoing[i]];
          wrap(call, s.path);
        }
      }
    }
  }
  void processSummary(std::size_t id, PathId path) {
    auto s = summaries_[id];
    if (s.path != path || s.processed == path)
      return;
    summaries_[id].processed = path;
    auto &source = nodes_.at(s.source);
    auto &target = nodes_.at(s.target);
    auto count = source.incoming.size();
    for (std::size_t i = 0; i < count; ++i) {
      context_->check();
      auto left = summaries_[source.incoming[i]].path;
      combine(left, path);
    }
    count = target.outgoing.size();
    for (std::size_t i = 0; i < count; ++i) {
      context_->check();
      auto right = summaries_[target.outgoing[i]].path;
      combine(path, right);
    }
    count = source.callers.size();
    for (std::size_t i = 0; i < count; ++i) {
      context_->check();
      auto call = source.callers[i];
      wrap(call, path);
    }
    for (unsigned above = 0; above < 2; ++above)
      if (source.prefix[above] != None)
        extend(s.target, above, source.prefix[above], path);
  }
  NestedRun run(PathId id) {
    NestedRun result;
    if (paths_[id].length > context_->maxPairs)
      throw ResourceLimit();
    result.states.reserve(paths_[id].length + 1);
    result.word.reserve(paths_[id].length);
    result.states.push_back(paths_[id].source);
    std::pmr::vector<PathId> work(workspace_.resource());
    work.push_back(id);
    {
      detail::HotLoopScope hot(*context_, "finite-witness-expansion");
      while (!work.empty()) {
        context_->check();
        auto path = paths_[work.back()];
        work.pop_back();
        if (path.left != None) {
          work.push_back(path.right);
          work.push_back(path.left);
        } else if (path.nonempty) {
          if (result.word.size() >= context_->maxPairs)
            throw ResourceLimit();
          result.word.push_back(path.letter);
          result.states.push_back(path.target);
        }
      }
    }
    result.computeMatching(operand_->alphabet());
    return result;
  }

public:
  LazySearch(Provider operand, SearchOptions options, Context context)
      : operand_(std::move(operand)), options_(std::move(options)),
        context_(std::move(context)), workspace_(context_->maxWorkspaceBytes),
        nodes_(workspace_.resource()), paths_(workspace_.resource()),
        summaries_(workspace_.resource()), index_(workspace_.resource()),
        agenda_(std::greater<Task>{},
                std::pmr::vector<Task>(workspace_.resource())) {}
  EmptinessResult search() {
    PathId accepted = None;
    {
      detail::HotLoopScope hot(*context_, "lazy-finite-search");
      for (State q : operand_->initialStates()) {
        context_->check();
        prefix(q, false, node(q).empty);
      }
      while (!agenda_.empty()) {
        context_->check();
        auto task = agenda_.top();
        agenda_.pop();
        if (task.summary) {
          processSummary(task.id, task.path);
          continue;
        }
        State q = static_cast<State>(task.id / 2);
        bool above = task.id % 2;
        auto &current = nodes_.at(q);
        if (current.prefix[above] != task.path)
          continue;
        if (operand_->isFinal(q) &&
            (options_.semantics == WordSemantics::Open || !above)) {
          accepted = task.path;
          break;
        }
        expand(q);
        auto count = current.outgoing.size();
        for (std::size_t i = 0; i < count; ++i) {
          context_->check();
          auto s = summaries_[current.outgoing[i]];
          extend(s.target, above, task.path, s.path);
        }
        for (const auto &call : current.calls) {
          context_->check();
          extend(call.target, true, task.path, call.path);
        }
        if (!above && options_.semantics == WordSemantics::Open) {
          if (!current.bottomExpanded) {
            current.bottomExpanded = true;
            for (Letter r :
                 operand_->enabledLettersView(q, LetterKind::Return, Bottom)) {
              context_->check();
              for (State t : operand_->successorsView(q, r, Bottom)) {
                context_->check();
                node(t);
                current.pendingReturns.push_back(edge({q, r, t, Bottom}));
              }
            }
          }
          for (auto path : current.pendingReturns) {
            context_->check();
            extend(paths_[path].target, false, task.path, path);
          }
        }
      }
    }
    if (accepted != None)
      return {run(accepted), paths_[accepted].cost, summaries_.size()};
    return {std::nullopt, 0, summaries_.size()};
  }
};
} // namespace

EmptinessResult isEmpty(Provider operand, const SearchOptions &options,
                        Context context) {
  if (!operand || !context)
    throw std::invalid_argument("null search operand or context");
  context->check();
  return LazySearch(std::move(operand), options, std::move(context)).search();
}
} // namespace lotus::verification::automata
