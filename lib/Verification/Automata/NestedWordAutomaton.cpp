#include "Verification/Automata/NestedWordAutomaton.h"

#include "Verification/Automata/Detail/Workspace.h"
#include "Verification/Automata/NwaOperations.h"

#include <algorithm>

namespace lotus::verification::automata {
namespace {
const std::vector<State> Empty;
std::uint64_t key(Letter letter, State hierarchy) {
  return (std::uint64_t(hierarchy) << 32) | letter;
}
} // namespace

void OperationContext::check() const {
  if ((cancel && cancel->load(std::memory_order_relaxed)) ||
      (deadline != std::chrono::steady_clock::time_point::max() &&
       std::chrono::steady_clock::now() >= deadline))
    throw OperationCancelled();
}
Context defaultContext() {
  static const auto context = std::make_shared<const OperationContext>();
  return context;
}

Letter Alphabet::intern(const std::string &name, LetterKind kind) {
  const auto found = ids_.find(name);
  if (found != ids_.end()) {
    if (kinds_[found->second] != kind)
      throw std::invalid_argument("letter occurs in multiple partitions");
    return found->second;
  }
  if (names_.size() >= Bottom)
    throw ResourceLimit();
  const auto id = static_cast<Letter>(names_.size());
  ids_.emplace(name, id);
  names_.push_back(name);
  kinds_.push_back(kind);
  partitions_[static_cast<unsigned>(kind)].push_back(id);
  return id;
}
LetterKind Alphabet::kind(Letter letter) const { return kinds_.at(letter); }
const std::string &Alphabet::name(Letter letter) const {
  return names_.at(letter);
}
const std::vector<Letter> &Alphabet::letters(LetterKind kind) const {
  return partitions_[static_cast<unsigned>(kind)];
}

NestedWordAutomaton::NestedWordAutomaton(Alphabet alphabet)
    : alphabet_(std::move(alphabet)) {}
State NestedWordAutomaton::addState(bool initial, bool accepting) {
  if (final_.size() >= Bottom)
    throw ResourceLimit();
  State id = static_cast<State>(final_.size());
  final_.push_back(accepting);
  edges_.emplace_back();
  letters_.emplace_back();
  if (initial)
    initial_.push_back(id);
  return id;
}
void NestedWordAutomaton::setFinal(State state, bool accepting) {
  final_.at(state) = accepting;
}
bool NestedWordAutomaton::isFinal(State state) const {
  return final_.at(state);
}
void NestedWordAutomaton::addTransition(State source, Letter letter,
                                        State target, State hierarchy) {
  if (source >= stateCount() || target >= stateCount() ||
      (hierarchy != Bottom && hierarchy >= stateCount()))
    throw std::out_of_range("invalid NWA transition state");
  if (alphabet_.kind(letter) != LetterKind::Return && hierarchy != Bottom)
    throw std::invalid_argument("only returns have hierarchical predecessors");
  auto &list = edges_[source][key(letter, hierarchy)];
  if (list.empty())
    letters_[source]
            [key(static_cast<Letter>(alphabet_.kind(letter)), hierarchy)]
                .push_back(letter);
  if (std::find(list.begin(), list.end(), target) == list.end()) {
    list.push_back(target);
    transitions_.push_back({source, letter, target, hierarchy});
  }
}
const std::vector<State> &
NestedWordAutomaton::successors(State state, Letter letter, State hierarchy) {
  return static_cast<const NestedWordAutomaton &>(*this).successors(
      state, letter, hierarchy);
}
const std::vector<State> &
NestedWordAutomaton::successors(State state, Letter letter,
                                State hierarchy) const {
  if (alphabet_.kind(letter) != LetterKind::Return)
    hierarchy = Bottom;
  const auto &edges = edges_.at(state);
  const auto it = edges.find(key(letter, hierarchy));
  return it == edges.end() ? Empty : it->second;
}

const std::vector<Letter> &
NestedWordAutomaton::enabledLetters(State state, LetterKind kind,
                                    State hierarchy) {
  return static_cast<const NestedWordAutomaton &>(*this).enabledLetters(
      state, kind, hierarchy);
}
const std::vector<Letter> &
NestedWordAutomaton::enabledLetters(State state, LetterKind kind,
                                    State hierarchy) const {
  if (kind != LetterKind::Return)
    hierarchy = Bottom;
  const auto &letters = letters_.at(state);
  auto found = letters.find(key(static_cast<Letter>(kind), hierarchy));
  return found == letters.end() ? Empty : found->second;
}

void NestedRun::computeMatching(const Alphabet &alphabet) {
  matching.assign(word.size(), -1);
  std::vector<std::size_t> calls;
  calls.reserve(word.size());
  for (std::size_t i = 0; i < word.size(); ++i) {
    if (alphabet.kind(word[i]) == LetterKind::Call)
      calls.push_back(i);
    else if (alphabet.kind(word[i]) == LetterKind::Return && !calls.empty()) {
      matching[i] = static_cast<std::int64_t>(calls.back());
      matching[calls.back()] = static_cast<std::int64_t>(i);
      calls.pop_back();
    }
  }
}

bool accepts(Provider automaton, const std::vector<Letter> &word,
             WordSemantics semantics, Context context) {
  PowersetDeterminizer det(std::move(automaton), context);
  State current = det.initialStates().front();
  std::vector<State> stack;
  stack.reserve(word.size());
  detail::HotLoopScope hot(*context, "finite-membership");
  for (Letter letter : word) {
    context->check();
    auto kind = det.alphabet().kind(letter);
    State hierarchy = Bottom;
    if (kind == LetterKind::Call)
      stack.push_back(current);
    else if (kind == LetterKind::Return) {
      if (stack.empty()) {
        if (semantics == WordSemantics::Closed)
          return false;
      } else {
        hierarchy = stack.back();
        stack.pop_back();
      }
    }
    current = det.successorsView(current, letter, hierarchy).front();
  }
  return det.isFinal(current) &&
         (semantics == WordSemantics::Open || stack.empty());
}

bool validateRun(SuccessorProvider &automaton, const NestedRun &run,
                 WordSemantics semantics, Context context) {
  if (run.states.size() != run.word.size() + 1 ||
      run.matching.size() != run.word.size())
    return false;
  const auto &initial = automaton.initialStates();
  if (std::find(initial.begin(), initial.end(), run.states.front()) ==
      initial.end())
    return false;
  std::vector<std::size_t> stack;
  stack.reserve(run.word.size());
  for (std::size_t i = 0; i < run.word.size(); ++i) {
    context->check();
    if (run.states[i] >= automaton.stateCount() ||
        run.word[i] >= automaton.alphabet().size())
      return false;
    State h = Bottom;
    auto kind = automaton.alphabet().kind(run.word[i]);
    if (kind == LetterKind::Call) {
      stack.push_back(i);
    } else if (kind == LetterKind::Return && !stack.empty()) {
      auto call = stack.back();
      stack.pop_back();
      h = run.states[call];
      if (run.matching[i] != static_cast<std::int64_t>(call) ||
          run.matching[call] != static_cast<std::int64_t>(i))
        return false;
    } else if (run.matching[i] != -1 || (kind == LetterKind::Return &&
                                         semantics == WordSemantics::Closed)) {
      return false;
    }
    const auto &succ = automaton.successorsView(run.states[i], run.word[i], h);
    if (std::find(succ.begin(), succ.end(), run.states[i + 1]) == succ.end())
      return false;
  }
  for (auto call : stack)
    if (run.matching[call] != -1)
      return false;
  return run.states.back() < automaton.stateCount() &&
         automaton.isFinal(run.states.back()) &&
         (semantics == WordSemantics::Open || stack.empty());
}
} // namespace lotus::verification::automata
