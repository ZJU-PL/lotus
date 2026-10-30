#pragma once

#include "Verification/Automata/NwaOperations.h"

#include <optional>

namespace lotus::verification::automata {

enum class SearchStrategy {
  Shortest,
  LoopPenalizingAStar,
  SummaryReachability
};
struct SearchOptions {
  SearchStrategy strategy = SearchStrategy::Shortest;
  WordSemantics semantics = WordSemantics::Open;
  std::vector<State> loopHeads;
  std::uint64_t loopPenalty = 10;
  std::uint64_t callPenalty = 2;
  // Optional nonnegative edge cost, in addition to the unit transition cost.
  std::function<std::uint64_t(const Transition &)> extraCost;
  // A* lower bound in the same cost units. Must be admissible and zero at
  // goals. Empty means the admissible zero heuristic (Dijkstra).
  std::function<std::uint64_t(State)> heuristic;
};

struct EmptinessResult {
  std::optional<NestedRun> run;
  std::uint64_t cost = 0;
  std::size_t summaryCount = 0;
  bool empty() const { return !run.has_value(); }
};

EmptinessResult isEmpty(Provider operand, const SearchOptions &options = {},
                        Context context = defaultContext());

struct BuchiLasso {
  NestedRun stem;
  NestedRun loop;
  // Positive for a pumpable loop with pending calls; zero for bounded stacks.
  std::size_t stackGrowth = 0;
};
struct BuchiEmptinessResult {
  std::optional<BuchiLasso> lasso;
  bool empty() const { return !lasso.has_value(); }
};
// Finite final-state flags are interpreted as Büchi accepting states here.
BuchiEmptinessResult buchiIsEmpty(Provider operand,
                                  Context context = defaultContext());

// Validate the pumpable certificate form returned by buchiIsEmpty: a nonempty
// loop preserves its entry stack prefix and either has zero net stack growth
// or pushes pending calls. Pending returns are permitted only at the bottom.
// Replays the stem and two loop copies and verifies matching positions,
// repeated accepting visits, frame preservation, and the claimed growth.
bool validateLasso(SuccessorProvider &operand, const BuchiLasso &lasso,
                   Context context = defaultContext());

} // namespace lotus::verification::automata
