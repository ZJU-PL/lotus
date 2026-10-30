#include "Verification/Automata/IncrementalInclusion.h"

namespace lotus::verification::automata {
IncrementalInclusionCheck::IncrementalInclusionCheck(Provider minuend,
                                                     Context context)
    : base_(std::move(minuend)), remainder_(base_),
      context_(std::move(context)) {
  if (!base_ || !context_)
    throw std::invalid_argument("null inclusion operand or context");
}
EmptinessResult IncrementalInclusionCheck::checkCandidate(Provider candidate) {
  context_->check();
  if (!candidate)
    throw std::invalid_argument("null inclusion candidate");
  auto found = candidates_.find(candidate.get());
  if (found == candidates_.end()) {
    auto diff = difference(base_, candidate, {}, context_);
    found = candidates_.emplace(candidate.get(), Candidate{candidate, diff, {}})
                .first;
  }
  if (!found->second.result)
    found->second.result = isEmpty(found->second.difference, {}, context_);
  return *found->second.result;
}
EmptinessResult IncrementalInclusionCheck::check() {
  context_->check();
  if (!result_)
    result_ = isEmpty(remainder_, {}, context_);
  return *result_;
}
EmptinessResult IncrementalInclusionCheck::add(Provider interpolant) {
  context_->check();
  // Construct and check before publishing, preserving the previous refinement
  // if cancellation or a resource limit interrupts the operation.
  auto next = difference(remainder_, interpolant, {}, context_);
  std::optional<EmptinessResult> result;
  if (result_) {
    if (result_->empty()) {
      result = *result_;
    } else if (auto lifted = liftDifferenceRun(next, *result_->run, context_)) {
      result = *result_;
      result->run = std::move(lifted);
    }
  }
  if (!result)
    result = isEmpty(next, {}, context_);
  remainder_ = std::move(next);
  result_ = *result;
  ++refinements_;
  return *result;
}
void IncrementalInclusionCheck::compact() {
  auto materialized = reachable(remainder_, context_).automaton;
  DelayedDeadEndRemoval prune(materialized, context_);
  prune.stage();
  auto compacted = prune.commit();
  remainder_ = std::move(compacted);
  // Witness state IDs change on compaction, so only language facts survive.
  if (result_ && !result_->empty())
    result_.reset();
}
} // namespace lotus::verification::automata
