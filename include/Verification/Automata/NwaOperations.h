#pragma once

#include "Verification/Automata/NestedWordAutomaton.h"

#include <functional>
#include <optional>
#include <utility>

namespace lotus::verification::automata {

using DoubleDecker = std::pair<State, State>;

// Hash-consed relations of (caller, current) states. The empty relation is an
// explicit rejecting sink; every alphabet letter has one successor.
class PowersetDeterminizer final : public SuccessorProvider {
public:
  explicit PowersetDeterminizer(Provider operand,
                                Context context = defaultContext(),
                                bool pruneRejectingStates = false);
  ~PowersetDeterminizer() override;
  const Alphabet &alphabet() const override;
  const std::vector<State> &initialStates() const override;
  std::size_t stateCount() const override;
  bool isFinal(State state) const override;
  const std::vector<State> &successors(State state, Letter letter,
                                       State hierarchy = Bottom) override;
  ArrayView<State> successorsView(State state, Letter letter,
                                  State hierarchy = Bottom) override;
  const std::vector<DoubleDecker> &relation(State state) const;
  ArrayView<DoubleDecker> relationView(State state) const;
  std::size_t cachedTransitionCount() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

Provider complement(Provider operand, Context context = defaultContext());
Provider intersection(Provider lhs, Provider rhs,
                      Context context = defaultContext());

struct DifferenceOptions {
  // Certify accepting traps from the explicit operand before pruning them.
  // An accepting state alone is never treated as an absorbing trap.
  bool finalIsTrap = false;
};
Provider difference(Provider lhs, Provider rhs, DifferenceOptions options = {},
                    Context context = defaultContext());

// Reuse a run of the left operand after a refinement, exploring only its word.
// Returns nullopt if the new subtrahend accepts that word. The provider must be
// the lazy product returned by difference(); all state IDs in the input run
// belong to its left operand, and output IDs belong to the product.
std::optional<NestedRun> liftDifferenceRun(Provider product,
                                           const NestedRun &leftRun,
                                           Context context = defaultContext());

struct ReachabilityResult {
  std::vector<DoubleDecker> configurations;
  std::shared_ptr<NestedWordAutomaton> automaton;
  // Original provider state corresponding to each materialized state.
  std::vector<State> originalStates;
};

// Finite saturation of reachable double-deckers; no bound on recursion depth.
// Only successors triggered by reachable operand transitions are requested.
ReachabilityResult reachable(Provider operand,
                             Context context = defaultContext());

} // namespace lotus::verification::automata
