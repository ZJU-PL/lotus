#pragma once

#include "Verification/Automata/NwaReduction.h"

namespace lotus::verification::automata {

// Owns immutable operands and retains lazy caches between queries.
// checkCandidate checks A <= I independently; add checks A <=
// union(I_1,...,I_k).
class IncrementalInclusionCheck {
public:
  explicit IncrementalInclusionCheck(Provider minuend,
                                     Context context = defaultContext());
  EmptinessResult checkCandidate(Provider candidate);
  EmptinessResult add(Provider interpolant);
  EmptinessResult check();
  // Materialize and prune at a caller-selected CEGAR checkpoint, releasing the
  // chain of previous difference providers and their determinization caches.
  void compact();
  Provider remainder() const { return remainder_; }
  std::size_t refinementCount() const { return refinements_; }

private:
  Provider base_, remainder_;
  Context context_;
  std::size_t refinements_ = 0;
  std::optional<EmptinessResult> result_;
  struct Candidate {
    Provider operand, difference;
    std::optional<EmptinessResult> result;
  };
  std::unordered_map<const SuccessorProvider *, Candidate> candidates_;
};

} // namespace lotus::verification::automata
