#pragma once

#include "Verification/Automata/Detail/Workspace.h"
#include "Verification/Automata/NwaSearch.h"

#include <array>
#include <queue>
#include <unordered_map>

namespace lotus::verification::automata::detail {
using PathId = std::size_t;
constexpr PathId NoPath = std::numeric_limits<PathId>::max();
constexpr std::uint64_t Infinity = std::numeric_limits<std::uint64_t>::max();

struct Path {
  State source, target;
  std::uint64_t cost;
  bool accepting, nonempty;
  PathId left = NoPath, right = NoPath;
  Letter letter = Bottom;
  std::size_t length = 0;
};
struct Summary {
  State source, target;
  unsigned flags;
  PathId path;
  bool settled = false;
};

// Weighted context-free closure. Entries distinguish accepting/nonaccepting
// and empty/nonempty runs so Büchi cycles cannot be fabricated from epsilon.
class SummaryGraph {
public:
  std::shared_ptr<NestedWordAutomaton> automaton;
  Context context;
  Workspace workspace;
  std::pmr::vector<Path> paths;
  std::pmr::vector<Summary> summaries;
  std::pmr::vector<std::pmr::vector<std::size_t>> outgoing, incoming;
  SearchOptions options;

  SummaryGraph(std::shared_ptr<NestedWordAutomaton> a, SearchOptions options,
               Context context);
  PathId epsilon(State q);
  PathId edge(const Transition &t);
  PathId concat(PathId left, PathId right);
  NestedRun expand(PathId path) const;
  std::uint64_t edgeCost(const Transition &t) const;
  void compute();

private:
  using QueueItem = std::pair<std::uint64_t, std::size_t>;
  std::priority_queue<QueueItem, std::pmr::vector<QueueItem>,
                      std::greater<QueueItem>>
      queue_;
  std::pmr::unordered_map<std::uint64_t, std::array<std::size_t, 4>> index_;
  std::pmr::vector<std::pmr::vector<Transition>> callsInto_;
  void offer(PathId path);
  bool improves(State source, State target, bool accepting, bool nonempty,
                std::uint64_t cost) const;
  void offerConcat(PathId left, PathId right);
};

struct Prefixes {
  // Vertices 2*q are at the bottom, 2*q+1 have at least one pending call.
  std::vector<PathId> paths;
  std::optional<std::size_t> goal;
};
Prefixes prefixes(SummaryGraph &graph, bool stopAtFinal);
std::uint64_t addCost(std::uint64_t a, std::uint64_t b);
} // namespace lotus::verification::automata::detail
