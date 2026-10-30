#include "Verification/Automata/Detail/Summary.h"
#include "Verification/Automata/NwaReduction.h"

#include <algorithm>
#include <unordered_set>

namespace lotus::verification::automata {
namespace {
class PrunedView final : public SuccessorProvider {
  std::shared_ptr<NestedWordAutomaton> operand_;
  std::shared_ptr<const std::vector<bool>> live_;
  Context context_;
  detail::Workspace workspace_;
  std::vector<State> initial_;
  // Per-state maps retain stable successor lists across subsequent queries.
  std::pmr::vector<
      std::pmr::unordered_map<std::uint64_t, std::pmr::vector<State>>>
      cache_;
  std::unordered_map<const State *, std::vector<State>> legacy_;

public:
  PrunedView(std::shared_ptr<NestedWordAutomaton> operand,
             std::shared_ptr<const std::vector<bool>> live, Context context)
      : operand_(std::move(operand)), live_(std::move(live)),
        context_(std::move(context)), workspace_(context_->maxWorkspaceBytes),
        cache_(workspace_.resource()) {
    cache_.resize(operand_->stateCount());
    for (State q : operand_->initialStates())
      if (live_->at(q))
        initial_.push_back(q);
  }
  const Alphabet &alphabet() const override { return operand_->alphabet(); }
  const std::vector<State> &initialStates() const override { return initial_; }
  std::size_t stateCount() const override { return operand_->stateCount(); }
  bool isFinal(State q) const override {
    return live_->at(q) && operand_->isFinal(q);
  }
  ArrayView<Letter> enabledLettersView(State q, LetterKind kind,
                                       State h) override {
    if (kind != LetterKind::Return)
      h = Bottom;
    if (!live_->at(q) || (h != Bottom && !live_->at(h)))
      return {};
    return operand_->enabledLettersView(q, kind, h);
  }
  const std::vector<State> &successors(State q, Letter a, State h) override {
    static const std::vector<State> empty;
    auto view = successorsView(q, a, h);
    if (view.empty())
      return empty;
    auto found = legacy_.find(view.data());
    if (found != legacy_.end())
      return found->second;
    return legacy_
        .emplace(view.data(), std::vector<State>(view.begin(), view.end()))
        .first->second;
  }
  ArrayView<State> successorsView(State q, Letter a, State h) override {
    context_->check();
    if (alphabet().kind(a) != LetterKind::Return)
      h = Bottom;
    auto key = (std::uint64_t(h) << 32) | a;
    auto &cache = cache_.at(q);
    auto it = cache.find(key);
    if (it != cache.end())
      return it->second;
    std::pmr::vector<State> result(workspace_.resource());
    if (live_->at(q) && (h == Bottom || live_->at(h)))
      for (State t : operand_->successorsView(q, a, h)) {
        context_->check();
        if (live_->at(t))
          result.push_back(t);
      }
    return cache.emplace(key, std::move(result)).first->second;
  }
};
} // namespace

DelayedDeadEndRemoval::DelayedDeadEndRemoval(
    std::shared_ptr<NestedWordAutomaton> operand, Context context)
    : operand_(std::move(operand)), context_(std::move(context)) {
  if (!operand_ || !context_)
    throw std::invalid_argument("null pruning operand or context");
}
const std::vector<bool> &DelayedDeadEndRemoval::liveStates() const {
  if (!live_)
    throw std::logic_error("dead-end removal has not been staged");
  return *live_;
}
void DelayedDeadEndRemoval::stage() {
  context_->check();
  detail::SummaryGraph g(operand_, {}, context_);
  g.compute();
  auto prefix = detail::prefixes(g, false);
  const auto n = operand_->stateCount();
  auto live = std::make_shared<std::vector<bool>>(n, false);
  // Reverse reachability in the graph of balanced summaries and pending edges.
  struct Arc {
    std::size_t source, target;
    bool summary;
  };
  std::vector<Arc> arcs;
  std::vector<std::vector<std::size_t>> incoming(2 * n);
  auto arc = [&](std::size_t s, std::size_t t, bool summary) {
    context_->check();
    incoming[t].push_back(arcs.size());
    arcs.push_back({s, t, summary});
  };
  // Boolean summary relation, independent of accepting/nonempty flags.
  std::vector<std::unordered_set<State>> balanced(n);
  for (const auto &s : g.summaries) {
    context_->check();
    balanced[s.source].insert(s.target);
  }
  for (State s = 0; s < n; ++s) {
    context_->check();
    for (State t : balanced[s]) {
      arc(2 * std::size_t(s), 2 * std::size_t(t), true);
      arc(2 * std::size_t(s) + 1, 2 * std::size_t(t) + 1, true);
    }
  }
  for (const auto &t : operand_->transitions()) {
    context_->check();
    auto kind = operand_->alphabet().kind(t.letter);
    if (kind == LetterKind::Call) {
      arc(2 * std::size_t(t.source), 2 * std::size_t(t.target) + 1, false);
      arc(2 * std::size_t(t.source) + 1, 2 * std::size_t(t.target) + 1, false);
    } else if (kind == LetterKind::Return && t.hierarchy == Bottom) {
      arc(2 * std::size_t(t.source), 2 * std::size_t(t.target), false);
    }
  }
  std::vector<bool> productive(2 * n);
  std::vector<std::size_t> queue;
  queue.reserve(2 * n);
  for (std::size_t v = 0; v < 2 * n; ++v) {
    context_->check();
    if (operand_->isFinal(static_cast<State>(v / 2))) {
      productive[v] = true;
      queue.push_back(v);
    }
  }
  {
    detail::HotLoopScope hot(*context_, "pruning-backward-reachability");
    for (std::size_t head = 0; head < queue.size(); ++head) {
      context_->check();
      for (auto id : incoming[queue[head]]) {
        auto source = arcs[id].source;
        if (!productive[source]) {
          productive[source] = true;
          queue.push_back(source);
        }
      }
    }
  }
  std::pmr::unordered_set<std::uint64_t> useful(g.workspace.resource());
  std::pmr::vector<DoubleDecker> work(g.workspace.resource());
  auto markSummary = [&](State s, State t) {
    context_->check();
    auto key = (std::uint64_t(s) << 32) | t;
    if (useful.insert(key).second)
      work.emplace_back(s, t);
    (*live)[s] = (*live)[t] = true;
  };
  for (std::size_t v = 0; v < 2 * n; ++v)
    if (productive[v] && prefix.paths[v] != detail::NoPath)
      (*live)[v / 2] = true;
  for (const auto &a : arcs) {
    context_->check();
    if (prefix.paths[a.source] != detail::NoPath && productive[a.target]) {
      (*live)[a.source / 2] = (*live)[a.target / 2] = true;
      if (a.summary)
        markSummary(static_cast<State>(a.source / 2),
                    static_cast<State>(a.target / 2));
    }
  }
  // Traverse every productive derivation, not only shortest witness paths.
  // Grammar: B[p,q] -> internal(p,r) B[r,q], or
  // call(p,c) B[c,u] return(u,p,r) B[r,q], or epsilon when p=q.
  {
    detail::HotLoopScope hot(*context_, "pruning-productive-summaries");
    for (std::size_t head = 0; head < work.size(); ++head) {
      context_->check();
      const auto [p, q] = work[head];
      for (Letter a : operand_->enabledLetters(p, LetterKind::Internal)) {
        context_->check();
        for (State r : operand_->successors(p, a))
          if (balanced[r].count(q))
            markSummary(r, q);
      }
      for (Letter c : operand_->enabledLetters(p, LetterKind::Call)) {
        context_->check();
        for (State entry : operand_->successors(p, c)) {
          context_->check();
          for (State exit : balanced[entry]) {
            context_->check();
            for (Letter r :
                 operand_->enabledLetters(exit, LetterKind::Return, p)) {
              context_->check();
              for (State target : operand_->successors(exit, r, p))
                if (balanced[target].count(q)) {
                  markSummary(entry, exit);
                  markSummary(target, q);
                }
            }
          }
        }
      }
    }
  }
  live_ = std::move(live); // Publish only after successful completion.
}
Provider DelayedDeadEndRemoval::view() const {
  liveStates();
  return std::make_shared<PrunedView>(operand_, live_, context_);
}
std::shared_ptr<NestedWordAutomaton> DelayedDeadEndRemoval::commit() const {
  const auto &live = liveStates();
  auto result = std::make_shared<NestedWordAutomaton>(operand_->alphabet());
  std::vector<State> ids(live.size(), Bottom);
  for (State q = 0; q < live.size(); ++q) {
    context_->check();
    if (live[q]) {
      const auto &initial = operand_->initialStates();
      ids[q] = result->addState(std::find(initial.begin(), initial.end(), q) !=
                                    initial.end(),
                                operand_->isFinal(q));
    }
  }
  for (const auto &t : operand_->transitions()) {
    context_->check();
    if (live[t.source] && live[t.target] &&
        (t.hierarchy == Bottom || live[t.hierarchy]))
      result->addTransition(ids[t.source], t.letter, ids[t.target],
                            t.hierarchy == Bottom ? Bottom : ids[t.hierarchy]);
  }
  return result;
}
} // namespace lotus::verification::automata
