#pragma once

#include "Verification/Automata/NwaSearch.h"

namespace lotus::verification::automata {

// Exact finite-language liveness mask. stage() computes a mask without mutating
// the operand; view() filters transitions on demand; commit() compacts once.
class DelayedDeadEndRemoval {
public:
  explicit DelayedDeadEndRemoval(std::shared_ptr<NestedWordAutomaton> operand,
                                 Context context = defaultContext());
  void stage();
  const std::vector<bool> &liveStates() const;
  Provider view() const;
  std::shared_ptr<NestedWordAutomaton> commit() const;

private:
  std::shared_ptr<NestedWordAutomaton> operand_;
  Context context_;
  std::shared_ptr<const std::vector<bool>> live_;
};

struct SimulationRelation {
  std::size_t states = 0;
  std::vector<std::uint64_t> bits;
  bool contains(State simulated, State simulator) const;
};

// Context-independent direct simulation checks both linear and hierarchical
// uses of a state. It is a sufficient, compositional NWA simulation preorder.
SimulationRelation directSimulation(const NestedWordAutomaton &operand,
                                    Context context = defaultContext());

// Exact direct simulation from empty paired stacks. Spoiler chooses a labelled
// transition; Duplicator must match it and every finite accepting prefix.
// Solves the unbounded pushdown safety game by alternating-automaton
// saturation. This rooted preorder alone is NOT a certificate for quotienting
// hierarchical states: callers may supply different stacks. Use the reduction
// API for a certified quotient, rather than merging this relation's symmetric
// pairs.
SimulationRelation directSimulationGame(const NestedWordAutomaton &operand,
                                        Context context = defaultContext());
std::shared_ptr<NestedWordAutomaton>
reduceDirectSimulation(const NestedWordAutomaton &operand,
                       Context context = defaultContext());

enum class AcceptanceSemantics { Finite, Buchi };
// Delayed Büchi game with an explicit outstanding-acceptance obligation bit.
// Hierarchical substitutions are restricted to equal return columns; the game
// checks every linear return context. This is a sound context-independent
// preorder, potentially smaller than full pushdown-game delayed simulation.
// Finite semantics makes stopping observable and requires immediate matching
// acceptance, since postponement does not preserve finite-word languages.
SimulationRelation delayedSimulation(const NestedWordAutomaton &operand,
                                     AcceptanceSemantics semantics,
                                     Context context = defaultContext());

// Exact delayed simulation from empty paired stacks, for Buchi acceptance.
// Carries acceptance debts through calls/returns and solves the pushdown Buchi
// game with nested greatest/least fixed points. Like directSimulationGame,
// this rooted relation is not an arbitrary-context quotient certificate.
SimulationRelation delayedSimulationGame(const NestedWordAutomaton &operand,
                                         Context context = defaultContext());
std::shared_ptr<NestedWordAutomaton>
reduceDelayedSimulation(const NestedWordAutomaton &operand,
                        AcceptanceSemantics semantics,
                        Context context = defaultContext());

// Propose quotients using the exact rooted games, then certify each accepted
// partition by a pushdown simulation from the quotient back to the operand.
// The certificate covers all caller contexts reachable from initial states.
// An invalid batch is refined; cancellation/limits propagate as exceptions.
// No determinization or full DFA minimization is used for these certificates.
std::shared_ptr<NestedWordAutomaton>
reduceSimulationGame(const NestedWordAutomaton &operand,
                     AcceptanceSemantics semantics,
                     Context context = defaultContext());

// Hopcroft refinement with an implicit rejecting sink. Rejects nondeterminism
// and any call/return transition; missing internal transitions are supported.
std::shared_ptr<NestedWordAutomaton>
minimizeDfa(const NestedWordAutomaton &operand,
            Context context = defaultContext());

} // namespace lotus::verification::automata
