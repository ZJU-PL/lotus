#include "Verification/Automata/Detail/Workspace.h"
#include "Verification/Automata/NwaReduction.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <numeric>
#include <unordered_set>

namespace lotus::verification::automata {
namespace {
std::uint64_t key(State first, State second) {
  return (std::uint64_t(first) << 32) | second;
}
void setPair(SimulationRelation &relation, State p, State q, bool present) {
  auto bit = std::size_t(p) * relation.states + q;
  auto mask = std::uint64_t(1) << (bit % 64);
  if (present)
    relation.bits[bit / 64] |= mask;
  else
    relation.bits[bit / 64] &= ~mask;
}

// Ignoring returns yields an upper bound; failed pairs lose independently of
// the stack. The existing context-independent relation supplies a lower bound.
SimulationRelation upperBound(const NestedWordAutomaton &a, Context context,
                              bool immediateAcceptance = true) {
  auto n = a.stateCount();
  SimulationRelation result{
      n, std::vector<std::uint64_t>((n * n + 63) / 64, ~std::uint64_t(0))};
  for (State p = 0; p < n; ++p)
    for (State q = 0; q < n; ++q) {
      context->check();
      if (immediateAcceptance && a.isFinal(p) && !a.isFinal(q))
        setPair(result, p, q, false);
    }
  bool changed = true;
  while (changed) {
    context->check();
    changed = false;
    for (State p = 0; p < n; ++p)
      for (State q = 0; q < n; ++q) {
        context->check();
        if (!result.contains(p, q))
          continue;
        bool valid = true;
        for (Letter letter = 0; letter < a.alphabet().size() && valid;
             ++letter) {
          context->check();
          if (a.alphabet().kind(letter) == LetterKind::Return)
            continue;
          for (State left : a.successors(p, letter)) {
            context->check();
            bool match = false;
            for (State right : a.successors(q, letter)) {
              context->check();
              if (result.contains(left, right)) {
                match = true;
                break;
              }
            }
            if (!match) {
              valid = false;
              break;
            }
          }
        }
        if (!valid) {
          setPair(result, p, q, false);
          changed = true;
        }
      }
  }
  return result;
}

// Each alphabet letter encodes Spoiler's choice (original letter, successor).
// All successors on that letter are Duplicator's possible responses. Both
// players' call sites are stored in the paired hierarchical state.
class DirectGame final : public SuccessorProvider {
  const NestedWordAutomaton &operand_;
  SimulationRelation upper_;
  Context context_;
  detail::Workspace workspace_;
  State size_, losing_;
  Alphabet alphabet_;
  std::vector<State> initial_;
  std::pmr::vector<
      std::pmr::unordered_map<std::uint64_t, std::pmr::vector<State>>>
      cache_;
  std::pmr::vector<
      std::pmr::unordered_map<std::uint64_t, std::pmr::vector<Letter>>>
      enabled_;
  std::unordered_map<const State *, std::vector<State>> legacySuccessors_;
  std::unordered_map<const Letter *, std::vector<Letter>> legacyLetters_;
  std::size_t queries_ = 0;

public:
  DirectGame(const NestedWordAutomaton &a, SimulationRelation upper,
             const SimulationRelation &lower, Context context,
             const std::vector<DoubleDecker> *roots = nullptr)
      : operand_(a), upper_(std::move(upper)), context_(std::move(context)),
        workspace_(context_->maxWorkspaceBytes),
        size_(static_cast<State>(a.stateCount())), losing_(size_ * size_),
        cache_(workspace_.resource()), enabled_(workspace_.resource()) {
    cache_.resize(std::size_t(losing_) + 1);
    enabled_.resize(size_);
    for (Letter a = 0; a < operand_.alphabet().size(); ++a)
      for (State target = 0; target < size_; ++target) {
        context_->check();
        alphabet_.intern(std::to_string(a) + ":" + std::to_string(target),
                         operand_.alphabet().kind(a));
      }
    auto addRoot = [&](State p, State q) {
      context_->check();
      if (upper_.contains(p, q) && !lower.contains(p, q))
        initial_.push_back(p * size_ + q);
    };
    if (roots) {
      for (auto root : *roots)
        addRoot(root.first, root.second);
    } else {
      for (State p = 0; p < size_; ++p)
        for (State q = 0; q < size_; ++q)
          addRoot(p, q);
    }
  }
  const Alphabet &alphabet() const override { return alphabet_; }
  const std::vector<State> &initialStates() const override { return initial_; }
  std::size_t stateCount() const override { return std::size_t(losing_) + 1; }
  bool isFinal(State q) const override {
    return q == losing_ || !upper_.contains(q / size_, q % size_);
  }
  const std::vector<Letter> &enabledLetters(State q, LetterKind kind,
                                            State h) override {
    static const std::vector<Letter> empty;
    auto view = enabledLettersView(q, kind, h);
    if (view.empty())
      return empty;
    auto found = legacyLetters_.find(view.data());
    if (found != legacyLetters_.end())
      return found->second;
    return legacyLetters_
        .emplace(view.data(), std::vector<Letter>(view.begin(), view.end()))
        .first->second;
  }
  ArrayView<Letter> enabledLettersView(State q, LetterKind kind,
                                       State h) override {
    static const std::vector<Letter> empty;
    if (isFinal(q))
      return empty;
    State p = q / size_;
    State hierarchy =
        kind != LetterKind::Return || h == Bottom ? Bottom : h / size_;
    auto code = key(hierarchy, static_cast<State>(kind));
    auto &cache = enabled_[p];
    auto found = cache.find(code);
    if (found != cache.end())
      return found->second;
    std::pmr::vector<Letter> result(workspace_.resource());
    for (Letter letter : operand_.enabledLetters(p, kind, hierarchy)) {
      context_->check();
      for (State target : operand_.successors(p, letter, hierarchy)) {
        context_->check();
        result.push_back(letter * size_ + target);
      }
    }
    detail::checkedSort(result.begin(), result.end(), *context_);
    return cache.emplace(code, std::move(result)).first->second;
  }
  const std::vector<State> &successors(State q, Letter letter,
                                       State h) override {
    static const std::vector<State> empty;
    auto view = successorsView(q, letter, h);
    if (view.empty())
      return empty;
    auto found = legacySuccessors_.find(view.data());
    if (found != legacySuccessors_.end())
      return found->second;
    return legacySuccessors_
        .emplace(view.data(), std::vector<State>(view.begin(), view.end()))
        .first->second;
  }
  ArrayView<State> successorsView(State q, Letter challenge, State h) override {
    context_->check();
    if (alphabet_.kind(challenge) != LetterKind::Return)
      h = Bottom;
    auto code = key(h, challenge);
    auto &cache = cache_.at(q);
    auto found = cache.find(code);
    if (found != cache.end())
      return found->second;
    if (queries_ >= context_->maxTransitions)
      throw ResourceLimit();
    std::pmr::vector<State> result(workspace_.resource());
    if (!isFinal(q)) {
      Letter letter = challenge / size_;
      State choice = challenge % size_;
      State rightHier = h == Bottom ? Bottom : h % size_;
      const auto choices = enabledLettersView(q, alphabet_.kind(challenge), h);
      if (std::binary_search(choices.begin(), choices.end(), challenge)) {
        for (State target : operand_.successors(q % size_, letter, rightHier)) {
          context_->check();
          result.push_back(choice * size_ + target);
        }
        if (result.empty())
          result.push_back(losing_);
      }
    }
    ++queries_;
    return cache.emplace(code, std::move(result)).first->second;
  }
};

class DelayedGame final : public SuccessorProvider {
  const NestedWordAutomaton &operand_;
  SimulationRelation upper_;
  Context context_;
  detail::Workspace workspace_;
  State size_, losing_;
  Alphabet alphabet_;
  std::vector<State> initial_;
  std::pmr::vector<
      std::pmr::unordered_map<std::uint64_t, std::pmr::vector<State>>>
      cache_;
  std::pmr::vector<
      std::pmr::unordered_map<std::uint64_t, std::pmr::vector<Letter>>>
      enabled_;
  std::unordered_map<const State *, std::vector<State>> legacySuccessors_;
  std::unordered_map<const Letter *, std::vector<Letter>> legacyLetters_;
  std::size_t queries_ = 0;

public:
  DelayedGame(const NestedWordAutomaton &a, SimulationRelation upper,
              Context context, const std::vector<DoubleDecker> *roots = nullptr)
      : operand_(a), upper_(std::move(upper)), context_(std::move(context)),
        workspace_(context_->maxWorkspaceBytes),
        size_(static_cast<State>(a.stateCount())), losing_(2 * size_ * size_),
        cache_(workspace_.resource()), enabled_(workspace_.resource()) {
    cache_.resize(std::size_t(losing_) + 1);
    enabled_.resize(size_);
    for (Letter letter = 0; letter < operand_.alphabet().size(); ++letter)
      for (State target = 0; target < size_; ++target) {
        context_->check();
        alphabet_.intern(std::to_string(letter) + ":" + std::to_string(target),
                         operand_.alphabet().kind(letter));
      }
    auto addRoot = [&](State p, State q) {
      context_->check();
      if (upper_.contains(p, q))
        initial_.push_back(2 * (p * size_ + q) +
                           (operand_.isFinal(p) && !operand_.isFinal(q)));
    };
    if (roots) {
      for (auto root : *roots)
        addRoot(root.first, root.second);
    } else {
      for (State p = 0; p < size_; ++p)
        for (State q = 0; q < size_; ++q)
          addRoot(p, q);
    }
  }
  const Alphabet &alphabet() const override { return alphabet_; }
  const std::vector<State> &initialStates() const override { return initial_; }
  std::size_t stateCount() const override { return std::size_t(losing_) + 1; }
  bool losing(State q) const {
    return q == losing_ || !upper_.contains((q / 2) / size_, (q / 2) % size_);
  }
  bool isFinal(State q) const override { return !losing(q) && q % 2 == 0; }
  const std::vector<Letter> &enabledLetters(State q, LetterKind kind,
                                            State h) override {
    static const std::vector<Letter> empty;
    auto view = enabledLettersView(q, kind, h);
    if (view.empty())
      return empty;
    auto found = legacyLetters_.find(view.data());
    if (found != legacyLetters_.end())
      return found->second;
    return legacyLetters_
        .emplace(view.data(), std::vector<Letter>(view.begin(), view.end()))
        .first->second;
  }
  ArrayView<Letter> enabledLettersView(State q, LetterKind kind,
                                       State h) override {
    static const std::vector<Letter> empty;
    if (losing(q))
      return empty;
    State p = (q / 2) / size_;
    State hierarchy =
        kind != LetterKind::Return || h == Bottom ? Bottom : (h / 2) / size_;
    auto code = key(hierarchy, static_cast<State>(kind));
    auto &cache = enabled_[p];
    auto found = cache.find(code);
    if (found != cache.end())
      return found->second;
    std::pmr::vector<Letter> result(workspace_.resource());
    for (Letter letter : operand_.enabledLetters(p, kind, hierarchy)) {
      context_->check();
      for (State target : operand_.successors(p, letter, hierarchy)) {
        context_->check();
        result.push_back(letter * size_ + target);
      }
    }
    detail::checkedSort(result.begin(), result.end(), *context_);
    return cache.emplace(code, std::move(result)).first->second;
  }
  const std::vector<State> &successors(State q, Letter letter,
                                       State h) override {
    static const std::vector<State> empty;
    auto view = successorsView(q, letter, h);
    if (view.empty())
      return empty;
    auto found = legacySuccessors_.find(view.data());
    if (found != legacySuccessors_.end())
      return found->second;
    return legacySuccessors_
        .emplace(view.data(), std::vector<State>(view.begin(), view.end()))
        .first->second;
  }
  ArrayView<State> successorsView(State q, Letter challenge, State h) override {
    context_->check();
    if (alphabet_.kind(challenge) != LetterKind::Return)
      h = Bottom;
    auto code = key(h, challenge);
    auto &cache = cache_.at(q);
    auto found = cache.find(code);
    if (found != cache.end())
      return found->second;
    if (queries_ >= context_->maxTransitions)
      throw ResourceLimit();
    std::pmr::vector<State> result(workspace_.resource());
    if (!losing(q)) {
      Letter letter = challenge / size_;
      State choice = challenge % size_;
      State rightHier = h == Bottom ? Bottom : (h / 2) % size_;
      const auto choices = enabledLettersView(q, alphabet_.kind(challenge), h);
      if (std::binary_search(choices.begin(), choices.end(), challenge)) {
        for (State target :
             operand_.successors((q / 2) % size_, letter, rightHier)) {
          context_->check();
          bool debt =
              (q % 2 || operand_.isFinal(choice)) && !operand_.isFinal(target);
          result.push_back(2 * (choice * size_ + target) + debt);
        }
        if (result.empty())
          result.push_back(losing_);
      }
    }
    ++queries_;
    return cache.emplace(code, std::move(result)).first->second;
  }
};

// Positive Boolean formulas over controls describe the remaining stack.
// DNF terms form an antichain under subset: X entails Y when X includes Y.
// Integer formula identities make fixed-point equality constant time.
class Formulas {
public:
  using Id = std::size_t;
  using Term = std::pmr::vector<State>;
  using Formula = std::pmr::vector<Term>;
  static constexpr Id False = 0, True = 1;

private:
  Context context_;
  std::pmr::memory_resource *memory_;
  std::pmr::deque<Formula> values_;
  std::pmr::unordered_map<std::size_t, std::pmr::vector<Id>> buckets_;
  std::size_t cells_ = 0;

  void insert(Formula &formula, Term term) {
    context_->check();
    for (const auto &existing : formula) {
      context_->check();
      if (std::includes(term.begin(), term.end(), existing.begin(),
                        existing.end()))
        return;
    }
    formula.erase(std::remove_if(formula.begin(), formula.end(),
                                 [&](const Term &existing) {
                                   context_->check();
                                   return std::includes(
                                       existing.begin(), existing.end(),
                                       term.begin(), term.end());
                                 }),
                  formula.end());
    if (formula.size() >= context_->maxPairs ||
        term.size() > context_->maxPairs)
      throw ResourceLimit();
    formula.push_back(std::move(term));
  }
  Id intern(Formula formula) {
    context_->check();
    detail::checkedSort(formula.begin(), formula.end(), *context_);
    std::size_t hash = formula.size(), cells = 1;
    for (const auto &term : formula) {
      context_->check();
      if (term.size() >= context_->maxPairs ||
          cells > context_->maxPairs - term.size() - 1)
        throw ResourceLimit();
      cells += term.size() + 1;
      hash ^= term.size() + 0x9e3779b9U + (hash << 6) + (hash >> 2);
      for (State q : term) {
        context_->check();
        hash ^= q + 0x9e3779b9U + (hash << 6) + (hash >> 2);
      }
    }
    auto found = buckets_.find(hash);
    if (found != buckets_.end())
      for (Id id : found->second) {
        context_->check();
        if (values_[id] == formula)
          return id;
      }
    if (cells_ > context_->maxPairs || cells > context_->maxPairs - cells_)
      throw ResourceLimit();
    Id id = values_.size();
    values_.push_back(std::move(formula));
    buckets_[hash].push_back(id);
    cells_ += cells;
    return id;
  }

public:
  Formulas(Context context, std::pmr::memory_resource *memory)
      : context_(std::move(context)), memory_(memory), values_(memory),
        buckets_(memory) {
    intern(Formula(memory_));
    Formula truth(memory_);
    truth.emplace_back();
    intern(std::move(truth));
  }
  const Formula &get(Id id) const { return values_.at(id); }
  Id atom(State q) {
    Term term(memory_);
    term.push_back(q);
    Formula formula(memory_);
    formula.push_back(std::move(term));
    return intern(std::move(formula));
  }
  Id either(Id a, Id b) {
    context_->check();
    if (a == b || b == False || a == True)
      return a;
    if (a == False || b == True)
      return b;
    Formula result(values_[a], memory_);
    for (const auto &term : values_[b])
      insert(result, Term(term, memory_));
    return intern(std::move(result));
  }
  Id both(Id a, Id b) {
    context_->check();
    if (a == b || b == True || a == False)
      return a;
    if (a == True || b == False)
      return b;
    Formula result(memory_);
    for (const auto &left : values_[a]) {
      context_->check();
      for (const auto &right : values_[b]) {
        context_->check();
        Term term(memory_);
        term.reserve(left.size() + right.size());
        std::set_union(left.begin(), left.end(), right.begin(), right.end(),
                       std::back_inserter(term));
        insert(result, std::move(term));
      }
    }
    return intern(std::move(result));
  }
  template <typename Rename> Id rename(Id id, Rename renameState) {
    context_->check();
    if (id == False || id == True)
      return id;
    Formula result(memory_);
    for (const auto &term : values_[id]) {
      context_->check();
      Term renamed(memory_);
      renamed.reserve(term.size());
      for (State q : term) {
        context_->check();
        renamed.push_back(renameState(q));
      }
      detail::checkedSort(renamed.begin(), renamed.end(), *context_);
      renamed.erase(
          detail::checkedUnique(renamed.begin(), renamed.end(), *context_),
          renamed.end());
      insert(result, std::move(renamed));
    }
    return intern(std::move(result));
  }
};

class SafetySaturation {
  std::shared_ptr<DirectGame> game_;
  Context context_;
  detail::Workspace workspace_;
  Formulas formulas_;
  std::vector<DoubleDecker> positions_;
  std::unordered_map<std::uint64_t, std::size_t> ids_;
  std::vector<Formulas::Id> values_;
  std::pmr::vector<std::pmr::vector<std::size_t>> dependents_;
  std::pmr::unordered_set<std::uint64_t> dependencies_;

  Formulas::Id read(State up, State down, std::size_t owner) {
    context_->check();
    if (game_->isFinal(up))
      return Formulas::True;
    auto it = ids_.find(key(up, down));
    if (it == ids_.end())
      throw std::logic_error("missing reachable simulation double-decker");
    auto dependency = it->second;
    if (dependency > Bottom || owner > Bottom)
      throw ResourceLimit();
    if (dependencies_
            .insert(
                key(static_cast<State>(dependency), static_cast<State>(owner)))
            .second) {
      if (dependencies_.size() > context_->maxTransitions)
        throw ResourceLimit();
      dependents_[dependency].push_back(owner);
    }
    return values_[dependency];
  }
  Formulas::Id substitute(Formulas::Id formula, State down, std::size_t owner) {
    auto result = Formulas::False;
    for (const auto &term : formulas_.get(formula)) {
      context_->check();
      auto conjunction = Formulas::True;
      for (State target : term) {
        context_->check();
        conjunction = formulas_.both(conjunction, read(target, down, owner));
        if (conjunction == Formulas::False)
          break;
      }
      result = formulas_.either(result, conjunction);
    }
    return result;
  }
  Formulas::Id evaluate(std::size_t id) {
    const auto [down, up] = positions_[id];
    if (game_->isFinal(up))
      return Formulas::True;
    auto result = values_[id];
    for (auto kind :
         {LetterKind::Call, LetterKind::Return, LetterKind::Internal}) {
      State h = kind == LetterKind::Return ? down : Bottom;
      for (Letter a : game_->enabledLettersView(up, kind, h)) {
        context_->check();
        const auto &responses = game_->successorsView(
            up, a, kind == LetterKind::Return ? down : Bottom);
        if (responses.empty())
          continue; // Spoiler cannot choose this challenge.
        auto challenge = Formulas::True;
        for (State target : responses) {
          context_->check();
          Formulas::Id response;
          if (game_->isFinal(target)) {
            response = Formulas::True;
          } else if (kind == LetterKind::Call) {
            // A push replaces gamma by up.gamma. Compose the alternating
            // transition on up with the transition on the previous top gamma.
            response = substitute(read(target, up, id), down, id);
          } else if (kind == LetterKind::Return && down != Bottom) {
            response = formulas_.atom(target);
          } else {
            response = read(target, down, id);
          }
          challenge = formulas_.both(challenge, response);
          if (challenge == Formulas::False)
            break;
        }
        result = formulas_.either(result, challenge);
        if (result == Formulas::True)
          return result;
      }
    }
    return result;
  }

public:
  SafetySaturation(std::shared_ptr<DirectGame> game, Context context)
      : game_(std::move(game)), context_(std::move(context)),
        workspace_(context_->maxWorkspaceBytes),
        formulas_(context_, workspace_.resource()),
        dependents_(workspace_.resource()),
        dependencies_(workspace_.resource()) {
    auto reach = reachable(game_, context_);
    positions_ = std::move(reach.configurations);
    values_.resize(positions_.size(), Formulas::False);
    dependents_.resize(positions_.size());
    for (std::size_t i = 0; i < positions_.size(); ++i) {
      context_->check();
      ids_.emplace(key(positions_[i].second, positions_[i].first), i);
      if (game_->isFinal(positions_[i].second))
        values_[i] = Formulas::True;
    }
  }
  void solve() {
    // Each position is queued at most once. Allocate the entire ring before
    // saturation; push/pop never allocate, including after many revisits.
    const auto size = positions_.size();
    if (!size)
      return;
    std::vector<std::size_t> queue(size);
    std::iota(queue.begin(), queue.end(), 0);
    std::vector<bool> pending(size, true);
    std::size_t head = 0, tail = 0, count = size;
    detail::HotLoopScope hot(*context_, "direct-simulation-fixed-point");
    while (count) {
      context_->check();
      auto id = queue[head];
      head = (head + 1) % size;
      --count;
      pending[id] = false;
      auto result = evaluate(id);
      if (result == values_[id])
        continue;
      values_[id] = result;
      for (auto dependent : dependents_[id]) {
        context_->check();
        if (!pending[dependent]) {
          queue[tail] = dependent;
          tail = (tail + 1) % size;
          ++count;
          pending[dependent] = true;
        }
      }
    }
  }
  bool loses(State root) const {
    return values_.at(ids_.at(key(root, Bottom))) == Formulas::True;
  }
};

// Carayol/Hague section 4.3: nu Z0 . mu Z1 .
// (F intersect CPre(Z0)) union (!F intersect CPre(Z1)).
// Positive formulas refer to priority-indexed control copies, not Boolean
// values at bounded stacks. Projecting copy 1 to copy 0 retains stack languages
// across the outer fixed point and summarizes arbitrarily many returns.
class BuchiSaturation {
  std::shared_ptr<DelayedGame> game_;
  Context context_;
  detail::Workspace workspace_;
  Formulas formulas_;
  State controls_;
  std::vector<DoubleDecker> positions_;
  std::unordered_map<std::uint64_t, std::size_t> ids_;
  std::vector<Formulas::Id> outer_, inner_, next_, projected_;
  std::pmr::unordered_map<Formulas::Id, Formulas::Id> projections_;

  Formulas::Id read(unsigned level, State up, State down) const {
    context_->check();
    if (game_->losing(up))
      return Formulas::False;
    auto found = ids_.find(key(up, down));
    if (found == ids_.end())
      throw std::logic_error("missing reachable delayed-game double-decker");
    return (level == 0 ? outer_ : inner_).at(found->second);
  }
  Formulas::Id substitute(Formulas::Id formula, State down) {
    auto result = Formulas::False;
    for (const auto &term : formulas_.get(formula)) {
      context_->check();
      auto conjunction = Formulas::True;
      for (State target : term) {
        context_->check();
        conjunction = formulas_.both(
            conjunction, read(target / controls_, target % controls_, down));
        if (conjunction == Formulas::False)
          break;
      }
      result = formulas_.either(result, conjunction);
    }
    return result;
  }
  Formulas::Id predecessor(std::size_t id) {
    const auto [down, up] = positions_[id];
    if (game_->losing(up))
      return Formulas::False;
    const unsigned level = game_->isFinal(up) ? 0 : 1;
    // Duplicator must answer every Spoiler challenge, but may choose a response
    // to each. A dead Spoiler position wins for Duplicator, even with debt.
    auto result = Formulas::True;
    for (auto kind :
         {LetterKind::Call, LetterKind::Return, LetterKind::Internal}) {
      State h = kind == LetterKind::Return ? down : Bottom;
      for (Letter a : game_->enabledLettersView(up, kind, h)) {
        context_->check();
        const auto &responses = game_->successorsView(
            up, a, kind == LetterKind::Return ? down : Bottom);
        if (responses.empty())
          continue;
        auto challenge = Formulas::False;
        for (State target : responses) {
          context_->check();
          Formulas::Id response;
          if (game_->losing(target))
            response = Formulas::False;
          else if (kind == LetterKind::Call)
            response = substitute(read(level, target, up), down);
          else if (kind == LetterKind::Return && down != Bottom)
            response = formulas_.atom(level * controls_ + target);
          else
            response = read(level, target, down);
          challenge = formulas_.either(challenge, response);
          if (challenge == Formulas::True)
            break;
        }
        result = formulas_.both(result, challenge);
        if (result == Formulas::False)
          return result;
      }
    }
    return result;
  }
  Formulas::Id project(Formulas::Id formula) {
    auto found = projections_.find(formula);
    if (found != projections_.end())
      return found->second;
    auto result = formulas_.rename(
        formula, [&](State q) { return q >= controls_ ? q - controls_ : q; });
    projections_.emplace(formula, result);
    return result;
  }

public:
  BuchiSaturation(std::shared_ptr<DelayedGame> game, Context context)
      : game_(std::move(game)), context_(std::move(context)),
        workspace_(context_->maxWorkspaceBytes),
        formulas_(context_, workspace_.resource()),
        controls_(static_cast<State>(game_->stateCount())),
        projections_(workspace_.resource()) {
    auto reach = reachable(game_, context_);
    positions_ = std::move(reach.configurations);
    for (std::size_t i = 0; i < positions_.size(); ++i) {
      context_->check();
      ids_.emplace(key(positions_[i].second, positions_[i].first), i);
    }
    outer_.assign(positions_.size(), Formulas::True);
    inner_.resize(positions_.size(), Formulas::False);
    next_.resize(positions_.size());
    projected_.resize(positions_.size());
  }
  void solve() {
    detail::HotLoopScope hot(*context_, "delayed-simulation-fixed-point");
    for (;;) {
      context_->check();
      std::fill(inner_.begin(), inner_.end(), Formulas::False);
      for (;;) {
        context_->check();
        for (std::size_t i = 0; i < positions_.size(); ++i) {
          context_->check();
          next_[i] = predecessor(i);
        }
        if (next_ == inner_)
          break;
        // PRE's temporary source copy 2 has no incoming references. Its
        // projection to copy 1 changes the source rows only.
        next_.swap(inner_);
      }
      for (std::size_t i = 0; i < positions_.size(); ++i) {
        context_->check();
        projected_[i] = project(inner_[i]);
      }
      if (projected_ == outer_)
        return;
      projected_.swap(outer_);
    }
  }
  bool wins(State root) const {
    return outer_.at(ids_.at(key(root, Bottom))) == Formulas::True;
  }
};
} // namespace

static SimulationRelation
directSimulationQueries(const NestedWordAutomaton &operand, Context context,
                        const std::vector<DoubleDecker> *roots) {
  if (!context)
    throw std::invalid_argument("null simulation context");
  context->check();
  auto n = operand.stateCount();
  auto lower = directSimulation(operand, context);
  if (!n)
    return lower;
  auto upper = upperBound(operand, context);
  if (lower.bits == upper.bits)
    return lower;
  if (n > (Bottom - 1) / n || n * n + 1 > context->maxStates ||
      operand.alphabet().size() > Bottom / n ||
      operand.alphabet().size() > context->maxTransitions / n)
    throw ResourceLimit();
  auto game = std::make_shared<DirectGame>(operand, std::move(upper), lower,
                                           context, roots);
  if (game->initialStates().empty())
    return lower;
  SafetySaturation saturation(game, context);
  saturation.solve();
  for (State root : game->initialStates()) {
    context->check();
    setPair(lower, root / static_cast<State>(n), root % static_cast<State>(n),
            !saturation.loses(root));
  }
  return lower;
}

static SimulationRelation
delayedSimulationQueries(const NestedWordAutomaton &operand, Context context,
                         const std::vector<DoubleDecker> *roots) {
  if (!context)
    throw std::invalid_argument("null delayed simulation context");
  context->check();
  const auto n = operand.stateCount();
  if (n && (n > (Bottom / 4 - 1) / n || n > (context->maxPairs / 2) / n ||
            2 * n * n + 1 > context->maxStates ||
            operand.alphabet().size() > Bottom / n ||
            operand.alphabet().size() > context->maxTransitions / n))
    throw ResourceLimit();
  SimulationRelation result{n, std::vector<std::uint64_t>((n * n + 63) / 64)};
  if (!n)
    return result;
  auto upper = upperBound(operand, context, false);
  auto game =
      std::make_shared<DelayedGame>(operand, std::move(upper), context, roots);
  if (game->initialStates().empty())
    return result;
  BuchiSaturation saturation(game, context);
  saturation.solve();
  for (State root : game->initialStates()) {
    context->check();
    const auto pair = root / 2;
    setPair(result, pair / static_cast<State>(n), pair % static_cast<State>(n),
            saturation.wins(root));
  }
  return result;
}

SimulationRelation directSimulationGame(const NestedWordAutomaton &operand,
                                        Context context) {
  return directSimulationQueries(operand, std::move(context), nullptr);
}

SimulationRelation delayedSimulationGame(const NestedWordAutomaton &operand,
                                         Context context) {
  return delayedSimulationQueries(operand, std::move(context), nullptr);
}

namespace {
std::shared_ptr<NestedWordAutomaton>
makeQuotient(const NestedWordAutomaton &operand,
             const std::vector<State> &partition, Context context) {
  auto result = std::make_shared<NestedWordAutomaton>(operand.alphabet());
  std::vector<State> blockIds(partition.size(), Bottom),
      mapping(partition.size());
  std::vector<bool> initial(partition.size()), final(partition.size());
  State count = 0;
  for (State q = 0; q < partition.size(); ++q) {
    context->check();
    auto &id = blockIds.at(partition[q]);
    if (id == Bottom)
      id = count++;
    mapping[q] = id;
    final[id] = final[id] || operand.isFinal(q);
  }
  for (State q : operand.initialStates()) {
    context->check();
    initial[mapping[q]] = true;
  }
  for (State q = 0; q < count; ++q) {
    context->check();
    result->addState(initial[q], final[q]);
  }
  for (const auto &t : operand.transitions()) {
    context->check();
    result->addTransition(mapping[t.source], t.letter, mapping[t.target],
                          t.hierarchy == Bottom ? Bottom
                                                : mapping[t.hierarchy]);
  }
  return result;
}

bool certifiesQuotient(const NestedWordAutomaton &original,
                       const NestedWordAutomaton &quotient,
                       AcceptanceSemantics semantics, Context context) {
  context->check();
  if (quotient.initialStates().empty())
    return true;
  if (original.initialStates().empty())
    return false;
  const auto total = original.stateCount() + quotient.stateCount();
  if (total >= Bottom || total > context->maxStates)
    throw ResourceLimit();
  NestedWordAutomaton combined(original.alphabet());
  State offset = 0;
  for (const auto *a : {&original, &quotient}) {
    for (State q = 0; q < a->stateCount(); ++q) {
      context->check();
      combined.addState(false, a->isFinal(q));
    }
    for (const auto &t : a->transitions()) {
      context->check();
      combined.addTransition(offset + t.source, t.letter, offset + t.target,
                             t.hierarchy == Bottom ? Bottom
                                                   : offset + t.hierarchy);
    }
    offset += static_cast<State>(a->stateCount());
  }
  offset = static_cast<State>(original.stateCount());
  std::vector<DoubleDecker> queries;
  for (State q : quotient.initialStates())
    for (State p : original.initialStates()) {
      context->check();
      if (queries.size() >= context->maxPairs)
        throw ResourceLimit();
      queries.emplace_back(offset + q, p);
    }
  // Original -> quotient is witnessed by the quotient homomorphism (including
  // hierarchical states). For the reverse direction, the original must match
  // all quotient runs under the paired stacks produced by those very runs.
  auto simulation = semantics == AcceptanceSemantics::Finite
                        ? directSimulationQueries(combined, context, &queries)
                        : delayedSimulationQueries(combined, context, &queries);
  for (State q : quotient.initialStates()) {
    context->check();
    bool covered = false;
    for (State p : original.initialStates()) {
      context->check();
      if (simulation.contains(offset + q, p)) {
        covered = true;
        break;
      }
    }
    if (!covered)
      return false;
  }
  return true;
}
} // namespace

std::shared_ptr<NestedWordAutomaton>
reduceSimulationGame(const NestedWordAutomaton &operand,
                     AcceptanceSemantics semantics, Context context) {
  if (!context)
    throw std::invalid_argument("null simulation reduction context");
  context->check();
  // The cheap compositional quotient is sound for both finite and Buchi words
  // and reduces the size of the subsequent pushdown games.
  auto base = reduceDirectSimulation(operand, context);
  const auto n = base->stateCount();
  if (n < 2)
    return base;
  auto relation = semantics == AcceptanceSemantics::Finite
                      ? directSimulationGame(*base, context)
                      : delayedSimulationGame(*base, context);
  std::vector<std::vector<State>> groups;
  std::vector<State> batch(n, Bottom);
  for (State p = 0; p < n; ++p) {
    context->check();
    if (batch[p] != Bottom)
      continue;
    groups.push_back({p});
    batch[p] = p;
    for (State q = p + 1; q < n; ++q) {
      context->check();
      if (relation.contains(p, q) && relation.contains(q, p)) {
        groups.back().push_back(q);
        batch[q] = p;
      }
    }
  }
  if (groups.size() == n)
    return base;
  auto candidate = makeQuotient(*base, batch, context);
  if (certifiesQuotient(*base, *candidate, semantics, context))
    return candidate;

  // A rooted equivalence can fail under a caller context introduced by merging
  // another state. Refine the failed batch and certify every accepted change.
  std::vector<State> partition(n), trial(n);
  std::iota(partition.begin(), partition.end(), 0);
  auto result = base;
  auto merge = [&](State p, State q) {
    State old = trial[q], replacement = trial[p];
    for (auto &block : trial) {
      context->check();
      if (block == old)
        block = replacement;
    }
  };
  auto accept = [&]() {
    auto proposal = makeQuotient(*base, trial, context);
    if (!certifiesQuotient(*base, *proposal, semantics, context))
      return false;
    partition = trial;
    result = std::move(proposal);
    return true;
  };
  for (const auto &group : groups) {
    context->check();
    if (group.size() < 2)
      continue;
    trial = partition;
    for (State q : group)
      merge(group.front(), q);
    if (accept())
      continue;
    for (std::size_t i = 1; i < group.size(); ++i)
      for (std::size_t j = 0; j < i; ++j) {
        context->check();
        if (partition[group[i]] == partition[group[j]])
          continue;
        trial = partition;
        merge(group[j], group[i]);
        accept();
      }
  }
  return result;
}
} // namespace lotus::verification::automata
