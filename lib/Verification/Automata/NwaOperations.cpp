#include "Verification/Automata/NwaOperations.h"

#include "Verification/Automata/Detail/Workspace.h"

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace lotus::verification::automata {
namespace {
std::uint64_t pairKey(State first, State second) {
  return (std::uint64_t(first) << 32) | second;
}
struct Query {
  State state, hierarchy;
  Letter letter;
  bool operator==(const Query &rhs) const {
    return state == rhs.state && hierarchy == rhs.hierarchy &&
           letter == rhs.letter;
  }
};
struct QueryHash {
  std::size_t operator()(const Query &q) const {
    return std::hash<std::uint64_t>{}(pairKey(q.state, q.hierarchy)) ^
           (std::size_t(q.letter) * 0x9e3779b9U);
  }
};
using LegacyCache = std::unordered_map<Query, std::vector<State>, QueryHash>;
using Cache =
    std::pmr::unordered_map<Query, std::pmr::vector<State>, QueryHash>;
const std::vector<State> EmptySuccessors;
struct RelationKey {
  ArrayView<DoubleDecker> relation;
  std::size_t hash;
};
struct RelationHash {
  std::size_t operator()(const RelationKey &key) const noexcept {
    return key.hash;
  }
};
struct RelationEqual {
  bool operator()(const RelationKey &a, const RelationKey &b) const noexcept {
    if (a.relation.size() != b.relation.size())
      return false;
    return a.relation.empty() ||
           std::equal(a.relation.begin(), a.relation.end(), b.relation.begin());
  }
};
void compatible(const Alphabet &a, const Alphabet &b, const Context &context) {
  if (a.size() != b.size())
    throw std::invalid_argument("NWA operators require the same alphabet");
  for (Letter l = 0; l < a.size(); ++l) {
    context->check();
    if (a.name(l) != b.name(l) || a.kind(l) != b.kind(l))
      throw std::invalid_argument("NWA operators require identical letter IDs");
  }
}
void checkStateLimit(std::size_t size, const Context &context) {
  context->check();
  if (size >= context->maxStates || size >= Bottom)
    throw ResourceLimit();
}
Context checkedContext(Context context) {
  if (!context)
    throw std::invalid_argument("null NWA context");
  return context;
}
std::vector<bool> rejectingStates(const Provider &operand,
                                  const Context &context) {
  auto explicitOperand =
      std::dynamic_pointer_cast<NestedWordAutomaton>(operand);
  if (!explicitOperand)
    return {};
  const auto n = explicitOperand->stateCount();
  if (n > context->maxStates ||
      explicitOperand->transitions().size() > context->maxTransitions)
    throw ResourceLimit();
  std::vector<bool> productive(n);
  std::vector<std::vector<State>> incoming(n);
  std::vector<State> queue;
  queue.reserve(n);
  for (State q = 0; q < n; ++q) {
    context->check();
    if (explicitOperand->isFinal(q)) {
      productive[q] = true;
      queue.push_back(q);
    }
  }
  for (const auto &edge : explicitOperand->transitions()) {
    context->check();
    incoming[edge.target].push_back(edge.source);
  }
  // Relaxing hierarchy overapproximates suffixes. States rejected even by
  // this relaxation cannot accept from any stack.
  detail::HotLoopScope hot(*context, "rejecting-trap-certification");
  for (std::size_t head = 0; head < queue.size(); ++head)
    for (State p : incoming[queue[head]]) {
      context->check();
      if (!productive[p]) {
        productive[p] = true;
        queue.push_back(p);
      }
    }
  for (State q = 0; q < n; ++q) {
    context->check();
    productive[q] = !productive[q];
  }
  return productive;
}
} // namespace

struct PowersetDeterminizer::Impl {
  Provider operand;
  Context context;
  std::vector<bool> rejecting;
  detail::Workspace workspace;
  std::pmr::deque<std::pmr::vector<DoubleDecker>> relations;
  std::pmr::unordered_map<RelationKey, State, RelationHash, RelationEqual> ids;
  std::vector<State> initial;
  Cache cache;
  const std::vector<State> sinkSuccessors{0};
  std::unordered_map<State, std::vector<State>> legacySuccessors;
  std::unordered_map<State, std::vector<DoubleDecker>> legacyRelations;
  // Reused across queries; operands have their own scratch storage.
  std::pmr::vector<DoubleDecker> scratch;
  std::size_t storedPairs = 0;

  Impl(Provider provider, Context operationContext, bool pruneRejecting)
      : operand(std::move(provider)), context(std::move(operationContext)),
        rejecting(pruneRejecting ? rejectingStates(operand, context)
                                 : std::vector<bool>{}),
        workspace(context->maxWorkspaceBytes), relations(workspace.resource()),
        ids(workspace.resource()), cache(workspace.resource()),
        scratch(workspace.resource()) {}

  State intern() {
    context->check();
    detail::checkedSort(scratch.begin(), scratch.end(), *context);
    scratch.erase(
        detail::checkedUnique(scratch.begin(), scratch.end(), *context),
        scratch.end());
    // Compute the cancellable linear hash before entering the hash table.
    // Container callbacks remain noexcept, so cancellation cannot interrupt a
    // rehash and leave published state IDs inconsistent.
    std::size_t hash = scratch.size();
    for (auto pair : scratch) {
      context->check();
      hash ^= std::hash<std::uint64_t>{}(pairKey(pair.first, pair.second)) +
              0x9e3779b9U + (hash << 6) + (hash >> 2);
    }
    auto found = ids.find(RelationKey{scratch, hash});
    if (found != ids.end())
      return found->second;
    checkStateLimit(relations.size(), context);
    if (storedPairs > context->maxPairs ||
        scratch.size() > context->maxPairs - storedPairs)
      throw ResourceLimit();
    State id = static_cast<State>(relations.size());
    relations.push_back(scratch);
    try {
      ids.emplace(RelationKey{relations.back(), hash}, id);
    } catch (...) {
      relations.pop_back();
      throw;
    }
    storedPairs += scratch.size();
    return id;
  }
  void append(State down, State up) {
    context->check();
    if (!rejecting.empty() && rejecting[up])
      return;
    if (scratch.size() >= context->maxPairs)
      throw ResourceLimit();
    scratch.emplace_back(down, up);
  }
};

PowersetDeterminizer::PowersetDeterminizer(Provider operand, Context context,
                                           bool pruneRejectingStates)
    : impl_(nullptr) {
  if (!operand || !context)
    throw std::invalid_argument("null NWA operand or context");
  impl_ = std::make_unique<Impl>(std::move(operand), std::move(context),
                                 pruneRejectingStates);
  // Reserve ID zero for the explicit bottom/sink relation, even when initial
  // states are present. This also makes repeated empty successors constant
  // time.
  impl_->intern();
  for (State q : impl_->operand->initialStates())
    impl_->append(Bottom, q);
  impl_->initial.push_back(impl_->intern());
}
PowersetDeterminizer::~PowersetDeterminizer() = default;
const Alphabet &PowersetDeterminizer::alphabet() const {
  return impl_->operand->alphabet();
}
const std::vector<State> &PowersetDeterminizer::initialStates() const {
  return impl_->initial;
}
std::size_t PowersetDeterminizer::stateCount() const {
  return impl_->relations.size();
}
bool PowersetDeterminizer::isFinal(State state) const {
  for (auto p : impl_->relations.at(state)) {
    impl_->context->check();
    if (impl_->operand->isFinal(p.second))
      return true;
  }
  return false;
}
const std::vector<DoubleDecker> &
PowersetDeterminizer::relation(State state) const {
  auto found = impl_->legacyRelations.find(state);
  if (found != impl_->legacyRelations.end())
    return found->second;
  const auto &value = impl_->relations.at(state);
  return impl_->legacyRelations
      .emplace(state, std::vector<DoubleDecker>(value.begin(), value.end()))
      .first->second;
}
ArrayView<DoubleDecker> PowersetDeterminizer::relationView(State state) const {
  return impl_->relations.at(state);
}
std::size_t PowersetDeterminizer::cachedTransitionCount() const {
  return impl_->cache.size();
}
const std::vector<State> &
PowersetDeterminizer::successors(State state, Letter letter, State hierarchy) {
  State target = successorsView(state, letter, hierarchy).front();
  if (target == 0)
    return impl_->sinkSuccessors;
  auto found = impl_->legacySuccessors.find(target);
  if (found != impl_->legacySuccessors.end())
    return found->second;
  return impl_->legacySuccessors.emplace(target, std::vector<State>{target})
      .first->second;
}
ArrayView<State> PowersetDeterminizer::successorsView(State state,
                                                      Letter letter,
                                                      State hierarchy) {
  auto &p = *impl_;
  p.context->check();
  auto kind = alphabet().kind(letter);
  if (kind != LetterKind::Return)
    hierarchy = Bottom;
  // An empty current relation or empty caller relation cannot recover on any
  // suffix. Share the sink response instead of allocating per-context entries.
  if (state == 0 || (kind == LetterKind::Return && hierarchy == 0))
    return p.sinkSuccessors;
  Query query{state, hierarchy, letter};
  auto found = p.cache.find(query);
  if (found != p.cache.end())
    return found->second;
  if (p.cache.size() >= p.context->maxTransitions)
    throw ResourceLimit();
  p.scratch.clear();
  const auto &linear = p.relations.at(state);
  if (kind != LetterKind::Return) {
    for (auto edge : linear) {
      p.context->check();
      for (State next : p.operand->successorsView(edge.second, letter))
        p.append(kind == LetterKind::Call ? edge.second : edge.first, next);
    }
  } else if (hierarchy == Bottom) {
    for (auto edge : linear) {
      p.context->check();
      if (edge.first == Bottom)
        for (State next :
             p.operand->successorsView(edge.second, letter, Bottom))
          p.append(Bottom, next);
    }
  } else {
    // Relations are sorted by down-state, so joining avoids a quadratic scan
    // over unrelated callee entries.
    for (auto caller : p.relations.at(hierarchy)) {
      p.context->check();
      auto it = std::lower_bound(linear.begin(), linear.end(),
                                 DoubleDecker{caller.second, 0});
      for (; it != linear.end() && it->first == caller.second; ++it) {
        p.context->check();
        for (State next :
             p.operand->successorsView(it->second, letter, caller.second))
          p.append(caller.first, next);
      }
    }
  }
  State target = p.intern();
  std::pmr::vector<State> result(p.workspace.resource());
  result.push_back(target);
  return p.cache.emplace(query, std::move(result)).first->second;
}

namespace {
class Complement final : public SuccessorProvider {
  PowersetDeterminizer det_;

public:
  Complement(Provider p, Context c) : det_(std::move(p), std::move(c)) {}
  const Alphabet &alphabet() const override { return det_.alphabet(); }
  const std::vector<State> &initialStates() const override {
    return det_.initialStates();
  }
  std::size_t stateCount() const override { return det_.stateCount(); }
  bool isFinal(State q) const override { return !det_.isFinal(q); }
  const std::vector<State> &successors(State q, Letter a, State h) override {
    return det_.successors(q, a, h);
  }
  ArrayView<State> successorsView(State q, Letter a, State h) override {
    return det_.successorsView(q, a, h);
  }
};

// A sound, context-independent certificate: from a certified state every
// letter, including a return with any caller, can stay in the accepting set.
std::vector<bool> universalStates(Provider provider, Context context) {
  auto nwa = std::dynamic_pointer_cast<NestedWordAutomaton>(provider);
  if (!nwa)
    throw std::invalid_argument(
        "finalIsTrap certification needs an explicit subtrahend");
  const auto n = nwa->stateCount();
  std::vector<bool> universal(n);
  for (State q = 0; q < n; ++q)
    universal[q] = nwa->isFinal(q);
  detail::HotLoopScope hot(*context, "accepting-trap-certification");
  bool changed = true;
  while (changed) {
    context->check();
    changed = false;
    for (State q = 0; q < n; ++q) {
      context->check();
      if (!universal[q])
        continue;
      for (Letter a = 0; a < nwa->alphabet().size() && universal[q]; ++a) {
        const bool ret = nwa->alphabet().kind(a) == LetterKind::Return;
        for (std::size_t i = 0; i < (ret ? n + 1 : 1); ++i) {
          context->check();
          State h = i == 0 ? Bottom : static_cast<State>(i - 1);
          const auto &succ = nwa->successors(q, a, h);
          if (std::none_of(succ.begin(), succ.end(),
                           [&](State t) { return universal[t]; })) {
            universal[q] = false;
            changed = true;
            break;
          }
        }
      }
    }
  }
  return universal;
}

class Product final : public SuccessorProvider {
  Provider lhs_, rhs_;
  std::shared_ptr<PowersetDeterminizer> det_;
  Context context_;
  detail::Workspace workspace_;
  bool difference_;
  std::vector<bool> universal_;
  std::pmr::deque<DoubleDecker> pairs_;
  std::pmr::unordered_map<std::uint64_t, State> ids_;
  std::vector<State> initial_;
  Cache cache_;
  LegacyCache legacyCache_;

  bool excluded(State right) const {
    if (universal_.empty())
      return false;
    for (auto p : det_->relationView(right)) {
      context_->check();
      if (universal_[p.second])
        return true;
    }
    return false;
  }
  State intern(State left, State right) {
    auto key = pairKey(left, right);
    auto it = ids_.find(key);
    if (it != ids_.end())
      return it->second;
    checkStateLimit(pairs_.size(), context_);
    State q = static_cast<State>(pairs_.size());
    pairs_.emplace_back(left, right);
    try {
      ids_.emplace(key, q);
    } catch (...) {
      pairs_.pop_back();
      throw;
    }
    return q;
  }

public:
  Product(Provider lhs, Provider rhs, bool subtract, DifferenceOptions options,
          Context context)
      : lhs_(std::move(lhs)), rhs_(std::move(rhs)),
        context_(checkedContext(std::move(context))),
        workspace_(context_->maxWorkspaceBytes), difference_(subtract),
        pairs_(workspace_.resource()), ids_(workspace_.resource()),
        cache_(workspace_.resource()) {
    if (!lhs_ || !rhs_ || !context_)
      throw std::invalid_argument("null NWA operand or context");
    compatible(lhs_->alphabet(), rhs_->alphabet(), context_);
    if (subtract) {
      if (options.finalIsTrap)
        universal_ = universalStates(rhs_, context_);
      det_ = std::make_shared<PowersetDeterminizer>(rhs_, context_,
                                                    options.finalIsTrap);
      rhs_ = det_;
    }
    for (State l : lhs_->initialStates()) {
      context_->check();
      for (State r : rhs_->initialStates())
        if (!excluded(r))
          initial_.push_back(intern(l, r));
    }
  }
  const Alphabet &alphabet() const override { return lhs_->alphabet(); }
  const std::vector<State> &initialStates() const override { return initial_; }
  std::size_t stateCount() const override { return pairs_.size(); }
  bool isFinal(State q) const override {
    const auto p = pairs_.at(q);
    return lhs_->isFinal(p.first) && (rhs_->isFinal(p.second) != difference_);
  }
  const std::vector<Letter> &enabledLetters(State q, LetterKind kind,
                                            State h) override {
    if (kind != LetterKind::Return)
      h = Bottom;
    const auto p = pairs_.at(q);
    const auto hierarchy = h == Bottom ? Bottom : pairs_.at(h).first;
    return lhs_->enabledLetters(p.first, kind, hierarchy);
  }
  ArrayView<Letter> enabledLettersView(State q, LetterKind kind,
                                       State h) override {
    if (kind != LetterKind::Return)
      h = Bottom;
    const auto p = pairs_.at(q);
    return lhs_->enabledLettersView(p.first, kind,
                                    h == Bottom ? Bottom : pairs_.at(h).first);
  }
  const std::vector<State> &successors(State q, Letter a, State h) override {
    if (alphabet().kind(a) != LetterKind::Return)
      h = Bottom;
    auto value = successorsView(q, a, h);
    if (value.empty())
      return EmptySuccessors;
    Query query{q, h, a};
    auto found = legacyCache_.find(query);
    if (found != legacyCache_.end())
      return found->second;
    return legacyCache_
        .emplace(query, std::vector<State>(value.begin(), value.end()))
        .first->second;
  }
  ArrayView<State> successorsView(State q, Letter a, State h) override {
    context_->check();
    if (alphabet().kind(a) != LetterKind::Return)
      h = Bottom;
    Query query{q, h, a};
    auto found = cache_.find(query);
    if (found != cache_.end())
      return found->second;
    if (cache_.size() >= context_->maxTransitions)
      throw ResourceLimit();
    auto p = pairs_.at(q);
    auto hp = h == Bottom ? DoubleDecker{Bottom, Bottom} : pairs_.at(h);
    const auto left = lhs_->successorsView(p.first, a, hp.first);
    std::pmr::vector<State> result(workspace_.resource());
    // Crucially do not query the determinizer if A cannot take this letter.
    if (!left.empty()) {
      const auto right = rhs_->successorsView(p.second, a, hp.second);
      for (State l : left) {
        context_->check();
        for (State r : right) {
          context_->check();
          if (!excluded(r))
            result.push_back(intern(l, r));
        }
      }
    }
    return cache_.emplace(query, std::move(result)).first->second;
  }
  std::optional<NestedRun> lift(const NestedRun &leftRun) {
    if (!difference_ ||
        !validateRun(*lhs_, leftRun, WordSemantics::Open, context_))
      throw std::invalid_argument("invalid minuend run for difference");
    State current = Bottom;
    for (State q : initial_) {
      context_->check();
      if (pairs_[q].first == leftRun.states.front()) {
        current = q;
        break;
      }
    }
    if (current == Bottom)
      return std::nullopt;
    NestedRun result;
    result.word = leftRun.word;
    result.matching = leftRun.matching;
    result.states.reserve(leftRun.states.size());
    result.states.push_back(current);
    std::vector<State> stack;
    stack.reserve(leftRun.word.size());
    for (std::size_t i = 0; i < leftRun.word.size(); ++i) {
      context_->check();
      Letter letter = leftRun.word[i];
      auto kind = alphabet().kind(letter);
      State hierarchy = Bottom;
      if (kind == LetterKind::Call)
        stack.push_back(current);
      else if (kind == LetterKind::Return && !stack.empty()) {
        hierarchy = stack.back();
        stack.pop_back();
      }
      State next = Bottom;
      for (State q : successorsView(current, letter, hierarchy)) {
        context_->check();
        if (pairs_[q].first == leftRun.states[i + 1]) {
          next = q;
          break;
        }
      }
      if (next == Bottom)
        return std::nullopt;
      current = next;
      result.states.push_back(current);
    }
    return isFinal(current) ? std::optional<NestedRun>(std::move(result))
                            : std::nullopt;
  }
};
} // namespace

Provider complement(Provider operand, Context context) {
  return std::make_shared<Complement>(std::move(operand), std::move(context));
}
Provider intersection(Provider lhs, Provider rhs, Context context) {
  return std::make_shared<Product>(std::move(lhs), std::move(rhs), false,
                                   DifferenceOptions{}, std::move(context));
}
Provider difference(Provider lhs, Provider rhs, DifferenceOptions options,
                    Context context) {
  return std::make_shared<Product>(std::move(lhs), std::move(rhs), true,
                                   options, std::move(context));
}
std::optional<NestedRun> liftDifferenceRun(Provider provider,
                                           const NestedRun &leftRun,
                                           Context context) {
  context->check();
  auto product = std::dynamic_pointer_cast<Product>(provider);
  if (!product)
    throw std::invalid_argument("expected a lazy difference product");
  return product->lift(leftRun);
}

ReachabilityResult reachable(Provider operand, Context context) {
  if (!operand || !context)
    throw std::invalid_argument("null NWA operand or context");
  context->check();
  detail::Workspace workspace(context->maxWorkspaceBytes);
  auto *memory = workspace.resource();
  std::pmr::vector<DoubleDecker> configurations(memory);
  std::pmr::vector<State> originals(memory);
  std::pmr::vector<Transition> transitions(memory);
  std::pmr::unordered_set<std::uint64_t> seen(memory), summarySeen(memory);
  std::pmr::unordered_set<State> initials(memory);
  std::pmr::unordered_map<State, State> ids(memory);
  std::pmr::unordered_map<State, std::pmr::vector<State>> parents(memory),
      summaries(memory);
  struct EdgeKey {
    State source, target, hierarchy;
    Letter letter;
    bool operator==(const EdgeKey &other) const {
      return source == other.source && target == other.target &&
             hierarchy == other.hierarchy && letter == other.letter;
    }
  };
  struct EdgeHash {
    std::size_t operator()(const EdgeKey &edge) const {
      return std::hash<std::uint64_t>{}(pairKey(edge.source, edge.target)) ^
             (std::hash<std::uint64_t>{}(pairKey(edge.hierarchy, edge.letter)) *
              0x9e3779b9U);
    }
  };
  std::pmr::unordered_set<EdgeKey, EdgeHash> edges(memory);
  auto state = [&](State q) {
    auto found = ids.find(q);
    if (found != ids.end())
      return found->second;
    checkStateLimit(ids.size(), context);
    State id = static_cast<State>(originals.size());
    ids.emplace(q, id);
    originals.push_back(q);
    return id;
  };
  auto enqueue = [&](State d, State u) {
    context->check();
    if (seen.insert(pairKey(d, u)).second) {
      if (configurations.size() >= context->maxPairs)
        throw ResourceLimit();
      state(u);
      configurations.emplace_back(d, u);
      parents[u].push_back(d);
    }
  };
  {
    detail::HotLoopScope hot(*context, "reachable-double-deckers");
    for (State q : operand->initialStates()) {
      initials.insert(q);
      enqueue(Bottom, q);
    }
    for (std::size_t head = 0; head < configurations.size(); ++head) {
      context->check();
      const auto [down, up] = configurations[head];
      auto known = summaries.find(up);
      if (known != summaries.end())
        for (State t : known->second)
          enqueue(down, t);
      for (auto kind :
           {LetterKind::Call, LetterKind::Return, LetterKind::Internal}) {
        State h = kind == LetterKind::Return ? down : Bottom;
        for (Letter a : operand->enabledLettersView(up, kind, h)) {
          context->check();
          for (State t : operand->successorsView(up, a, h)) {
            context->check();
            State from = state(up), to = state(t);
            State hierarchical = h == Bottom ? Bottom : state(h);
            if (edges.insert({from, to, hierarchical, a}).second) {
              if (transitions.size() >= context->maxTransitions)
                throw ResourceLimit();
              transitions.push_back({from, a, to, hierarchical});
            }
            if (kind == LetterKind::Call) {
              enqueue(up, t);
            } else if (kind == LetterKind::Internal || down == Bottom) {
              enqueue(down, t);
            } else if (summarySeen.insert(pairKey(down, t)).second) {
              summaries[down].push_back(t);
              auto count = parents[down].size();
              for (std::size_t i = 0; i < count; ++i) {
                State outer = parents[down][i];
                enqueue(outer, t);
              }
            }
          }
        }
      }
    }
  }
  // Materialize the public owning result after saturation. Output construction
  // is separate from the allocation-free exploration phase.
  ReachabilityResult result;
  result.automaton = std::make_shared<NestedWordAutomaton>(operand->alphabet());
  result.originalStates.assign(originals.begin(), originals.end());
  result.configurations.assign(configurations.begin(), configurations.end());
  for (State q : originals) {
    context->check();
    result.automaton->addState(initials.count(q) != 0, operand->isFinal(q));
  }
  for (const auto &t : transitions) {
    context->check();
    result.automaton->addTransition(t.source, t.letter, t.target, t.hierarchy);
  }
  return result;
}
} // namespace lotus::verification::automata
