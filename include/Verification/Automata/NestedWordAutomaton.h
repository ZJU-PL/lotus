#pragma once

#include "Verification/Automata/ArrayView.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotus::verification::automata {

using State = std::uint32_t;
using Letter = std::uint32_t;
constexpr State Bottom = std::numeric_limits<State>::max();
enum class LetterKind : std::uint8_t { Internal, Call, Return };

class OperationCancelled : public std::runtime_error {
public:
  OperationCancelled() : std::runtime_error("NWA operation cancelled") {}
};
class ResourceLimit : public std::runtime_error {
public:
  ResourceLimit() : std::runtime_error("NWA resource limit exceeded") {}
};

// Shared by composed operators. Exhaustion is never reported as emptiness.
struct OperationContext {
  const std::atomic<bool> *cancel = nullptr;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::time_point::max();
  std::size_t maxStates = 1000000;
  std::size_t maxPairs = 10000000;
  std::size_t maxWitnessNodes = 10000000;
  std::size_t maxTransitions = 10000000;
  std::size_t maxWorkspaceBytes = 256 * 1024 * 1024;
  // Optional profiler hook around named, allocation-audited hot phases.
  // The callback itself must not allocate or throw. Other phases may allocate.
  void (*hotLoopObserver)(const char *phase, bool entering,
                          void *data) noexcept = nullptr;
  void *observerData = nullptr;
  void check() const;
};
using Context = std::shared_ptr<const OperationContext>;
Context defaultContext();

// Letters have canonical integer identities within one immutable alphabet.
class Alphabet {
public:
  Letter intern(const std::string &name, LetterKind kind);
  LetterKind kind(Letter letter) const;
  const std::string &name(Letter letter) const;
  const std::vector<Letter> &letters(LetterKind kind) const;
  std::size_t size() const { return names_.size(); }

private:
  std::unordered_map<std::string, Letter> ids_;
  std::vector<std::string> names_;
  std::vector<LetterKind> kinds_;
  std::vector<Letter> partitions_[3];
};

// Queries may create states, but never invalidate previously returned successor
// lists. Providers are confined to one thread; cancellation may be
// cross-thread. Operands must not be modified during a provider's lifetime.
class SuccessorProvider {
public:
  virtual ~SuccessorProvider() = default;
  virtual const Alphabet &alphabet() const = 0;
  virtual const std::vector<State> &initialStates() const = 0;
  virtual std::size_t stateCount() const = 0;
  virtual bool isFinal(State state) const = 0;
  // A superset of the letters with outgoing transitions in this context.
  // Totalizing providers retain the full partition; sparse providers override
  // this to avoid querying every alphabet letter at every reachable state.
  virtual const std::vector<Letter> &enabledLetters(State, LetterKind kind,
                                                    State = Bottom) {
    return alphabet().letters(kind);
  }
  virtual const std::vector<State> &successors(State state, Letter letter,
                                               State hierarchy = Bottom) = 0;
  // Allocation-free query interface for arena-backed providers. The legacy
  // vector interface remains available for clients; algorithms use these views.
  virtual ArrayView<State> successorsView(State state, Letter letter,
                                          State hierarchy = Bottom) {
    return successors(state, letter, hierarchy);
  }
  virtual ArrayView<Letter> enabledLettersView(State state, LetterKind kind,
                                               State hierarchy = Bottom) {
    return enabledLetters(state, kind, hierarchy);
  }
};
using Provider = std::shared_ptr<SuccessorProvider>;

struct Transition {
  State source;
  Letter letter;
  State target;
  State hierarchy = Bottom;
};

class NestedWordAutomaton final : public SuccessorProvider {
public:
  explicit NestedWordAutomaton(Alphabet alphabet);
  State addState(bool initial = false, bool accepting = false);
  void setFinal(State state, bool accepting);
  void addTransition(State source, Letter letter, State target,
                     State hierarchy = Bottom);
  const Alphabet &alphabet() const override { return alphabet_; }
  const std::vector<State> &initialStates() const override { return initial_; }
  std::size_t stateCount() const override { return final_.size(); }
  bool isFinal(State state) const override;
  const std::vector<Letter> &enabledLetters(State state, LetterKind kind,
                                            State hierarchy = Bottom) override;
  const std::vector<Letter> &enabledLetters(State state, LetterKind kind,
                                            State hierarchy = Bottom) const;
  const std::vector<State> &successors(State state, Letter letter,
                                       State hierarchy = Bottom) override;
  const std::vector<State> &successors(State state, Letter letter,
                                       State hierarchy = Bottom) const;
  const std::vector<Transition> &transitions() const { return transitions_; }

private:
  Alphabet alphabet_;
  std::vector<bool> final_;
  std::vector<State> initial_;
  std::vector<std::unordered_map<std::uint64_t, std::vector<State>>> edges_;
  std::vector<std::unordered_map<std::uint64_t, std::vector<Letter>>> letters_;
  std::vector<Transition> transitions_;
};

struct NestedRun {
  std::vector<State> states; // Exactly word.size() + 1 entries.
  std::vector<Letter> word;
  // Position of matching call/return, or -1 for internal/pending positions.
  std::vector<std::int64_t> matching;
  void computeMatching(const Alphabet &alphabet);
};

// Open semantics are the default: final-state acceptance with any stack.
// Closed semantics prohibit pending returns as well as pending calls.
enum class WordSemantics { Open, Closed };
bool accepts(Provider automaton, const std::vector<Letter> &word,
             WordSemantics semantics = WordSemantics::Open,
             Context context = defaultContext());
bool validateRun(SuccessorProvider &automaton, const NestedRun &run,
                 WordSemantics semantics = WordSemantics::Open,
                 Context context = defaultContext());

} // namespace lotus::verification::automata
