#include "Checker/Pulse/Domain/PulseLoopAbstraction.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseJoin.h"
#include "Checker/Pulse/Report/PulseOptions.h"

#include <limits>
#include <optional>

#include <llvm/Analysis/LoopInfo.h>

namespace pulse {

namespace {

/**
 * Compute a sound widened invariant between `entry_state` (iteration k) and
 * `current_state` (iteration k+1) using PulseJoin + monotonic interval
 * widening on loop-carried integer variables, while preserving `current_state`'s
 * PHI entry predecessor.
 */
static std::optional<ExecutionDomain>
computeWidenedInvariant(const llvm::BasicBlock *header,
                        const ExecutionDomain &entry_state,
                        const ExecutionDomain &current_state) {
  const AbductiveDomain *entry_a = entry_state.getAstate();
  const AbductiveDomain *curr_a = current_state.getAstate();
  if (!entry_a || !curr_a) {
    return std::nullopt;
  }

  // Already structurally subsumed by entry state: inductive fixpoint!
  if (AbductiveDomain::leq(*curr_a, *entry_a)) {
    ExecutionDomain conv = entry_state.clone();
    conv.setEntryPred(current_state.getEntryPred());
    return conv;
  }

  auto joined_opt = PulseJoin::joinAbductive(*entry_a, *curr_a);
  if (!joined_opt) {
    return std::nullopt;
  }

  // Classic Cousot-Cousot monotonic interval widening for loop-carried
  // counters that moved strictly upward or downward across the iteration.
  const PulseFormula &ef = entry_a->getPathFormula();
  const PulseFormula &cf = curr_a->getPathFormula();
  PulseFormula &jf = joined_opt->getPathFormula();
  for (const auto &kv : joined_opt->getPostStack().getMap()) {
    const llvm::Value *var = kv.first;
    const Address *e_addr = entry_a->getPostStack().find(var);
    const Address *c_addr = curr_a->getPostStack().find(var);
    if (!e_addr || !c_addr) {
      continue;
    }
    auto e_lo = ef.getLowerBound(e_addr->addr);
    auto e_hi = ef.getUpperBound(e_addr->addr);
    auto c_lo = cf.getLowerBound(c_addr->addr);
    auto c_hi = cf.getUpperBound(c_addr->addr);
    AbstractValue j_av = joined_opt->getCanonical(kv.second.addr);
    if (e_lo && c_lo && *c_lo > *e_lo) {
      // Strictly increasing loop variable: keep lower bound, drop tight upper.
      (void)jf.addBounds(j_av, *e_lo, std::numeric_limits<int64_t>::max());
    } else if (e_hi && c_hi && *c_hi < *e_hi) {
      // Strictly decreasing loop variable: keep upper bound, drop tight lower.
      (void)jf.addBounds(j_av, std::numeric_limits<int64_t>::min(), *e_hi);
    }
  }

  // Attach LoopInvariantUnderInference marker so the next header visit checks
  // whether this candidate invariant is inductive (Infer PulseAbductiveDomain).
  joined_opt->startLoopInvariantInference(header, entry_a->clone());

  ExecutionDomain out = ExecutionDomain::continueProgram(
      std::make_unique<AbductiveDomain>(std::move(*joined_opt)));
  out.setEntryPred(current_state.getEntryPred());
  return out;
}

} // namespace

//===----------------------------------------------------------------------===//
// LoopAbstraction
//
// Provides widening/invariant inference scaffolding for the Pulse execution.
//
// Sound incorrectness guidance:
// - Loop handling is primarily a scalability concern. When we "widen", we
//   intentionally forget information to obtain termination, which can reduce
//   recall.
// - Path conditions are accumulated via disjunction-join (stable facts), never
//   conjunction, to avoid dropping feasible witness paths.
//===----------------------------------------------------------------------===//

void LoopAbstraction::initialize(const llvm::LoopInfo &LI) {
  loop_headers_.clear();

  // Collect all loops
  std::vector<const llvm::Loop *> loops;
  for (auto *L : LI) {
    loops.push_back(L);
    // Also collect subloops
    std::vector<const llvm::Loop *> worklist(L->begin(), L->end());
    while (!worklist.empty()) {
      const llvm::Loop *sub = worklist.back();
      worklist.pop_back();
      loops.push_back(sub);
      worklist.insert(worklist.end(), sub->begin(), sub->end());
    }
  }

  // Record loop headers and copy body blocks (safe after LLVM LoopInfo dies).
  for (const llvm::Loop *L : loops) {
    const llvm::BasicBlock *header = L->getHeader();
    if (header) {
      LoopInfo info;
      info.loop = L;
      info.body_blocks.insert(L->block_begin(), L->block_end());
      loop_headers_[header] = std::move(info);
    }
  }
}

bool LoopAbstraction::isLoopHeader(const llvm::BasicBlock *BB) const {
  return loop_headers_.count(BB) > 0;
}

const llvm::Loop *LoopAbstraction::getLoop(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  return (it != loop_headers_.end()) ? it->second.loop : nullptr;
}

bool LoopAbstraction::shouldWiden(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return false;
  }
  return it->second.iterations >= options::widenThreshold();
}

bool LoopAbstraction::visitHeader(const llvm::BasicBlock *BB,
                                  const ExecutionDomain &state) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return false;
  }

  it->second.iterations++;

  // Store state for comparison
  if (it->second.iterations == 1) {
    it->second.header_state = state.clone();
    it->second.entry_state = state.clone();
    it->second.seen_states.clear();
    it->second.seen_states.push_back(state.clone());
    auto *astate = state.getAstate();
    if (astate) {
      it->second.local_path_condition = astate->getPathFormula().clone();
    }
    initLoopInfo(BB);
    return false; // First iteration, no widening
  }

  // Record iteration with path stamp and bounded seen-state history.
  auto *astate = state.getAstate();
  if (astate) {
    unsigned timestamp = getNextTimestamp();
    PulseFormula path_condition = astate->getPathFormula().clone();
    pushLoopInfo(BB, timestamp, path_condition);

    // Update local path condition (disjunction join across iterations).
    it->second.local_path_condition =
        PulseFormula::join(it->second.local_path_condition, path_condition);
  }
  if (it->second.seen_states.size() < options::maxLoopIterations()) {
    it->second.seen_states.push_back(state.clone());
  }

  // Check if we should widen
  return it->second.iterations >= options::widenThreshold();
}

bool LoopAbstraction::hasExactRepeatingState(
    const llvm::BasicBlock *BB, const ExecutionDomain &state) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return false;
  }
  const AbductiveDomain *cand = state.getAstate();
  if (!cand || cand->isLoopInvariantUnderInference(BB)) {
    return false;
  }
  auto matchesAny = [&](const std::vector<ExecutionDomain> &history) {
    for (const auto &prev : history) {
      const AbductiveDomain *pa = prev.getAstate();
      if (!pa || pa->isLoopInvariantUnderInference(BB)) {
        continue;
      }
      if (AbductiveDomain::leq(*cand, *pa) &&
          AbductiveDomain::leq(*pa, *cand)) {
        return true;
      }
    }
    return false;
  };
  return matchesAny(it->second.seen_backedge_states) ||
         matchesAny(it->second.seen_states);
}

void LoopAbstraction::recordBackedgeState(const llvm::BasicBlock *BB,
                                          const ExecutionDomain &state) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return;
  }
  it->second.backedge_visits++;
  if (const AbductiveDomain *a = state.getAstate()) {
    if (!a->isLoopInvariantUnderInference(BB) &&
        it->second.seen_backedge_states.size() <
            options::maxLoopIterations()) {
      it->second.seen_backedge_states.push_back(state.clone());
    }
  }
}

unsigned LoopAbstraction::getBackedgeVisits(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  return (it != loop_headers_.end()) ? it->second.backedge_visits : 0u;
}

bool LoopAbstraction::hasConverged(const llvm::BasicBlock *BB,
                                   const ExecutionDomain &state) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return false;
  }
  const AbductiveDomain *cand = state.getAstate();
  if (!cand) {
    return false;
  }
  // If we were checking inductiveness of a candidate invariant and the next
  // iteration is subsumed by it, we have proved convergence.
  if (cand->isLoopInvariantUnderInference(BB) &&
      it->second.candidate_invariant &&
      it->second.candidate_invariant->getAstate()) {
    if (AbductiveDomain::leq(*cand,
                             *it->second.candidate_invariant->getAstate())) {
      return true;
    }
  }
  for (const auto &prev : it->second.seen_states) {
    if (prev.getEntryPred() == state.getEntryPred() && prev.getAstate() &&
        AbductiveDomain::leq(*cand, *prev.getAstate())) {
      return true;
    }
  }
  return false;
}

ExecutionDomain LoopAbstraction::widen(const llvm::BasicBlock *BB,
                                       const ExecutionDomain &current_state) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return current_state;
  }

  const ExecutionDomain &header_state = it->second.header_state;

  // Hard cap: stop exploring further back-edge unrollings once widened enough.
  if (it->second.iterations > options::maxLoopIterations()) {
    ExecutionDomain stopped;
    stopped.setState(ExecutionState::Stopped);
    return stopped;
  }

  // Prefer PulseJoin-based invariant widening preserving common loop mutations
  // and monotonic bounds; fall back to current/header state if join fails.
  if (auto inv = computeWidenedInvariant(BB, header_state, current_state)) {
    it->second.candidate_invariant = inv->clone();
    return std::move(*inv);
  }

  ExecutionDomain fallback = header_state.clone();
  fallback.setEntryPred(current_state.getEntryPred());
  return fallback;
}

bool LoopAbstraction::isInLoop(const llvm::BasicBlock *BB) const {
  for (const auto &kv : loop_headers_) {
    if (kv.second.body_blocks.count(BB) > 0) {
      return true;
    }
  }
  return false;
}

bool LoopAbstraction::isInLoopBody(const llvm::BasicBlock *header,
                                   const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(header);
  if (it == loop_headers_.end() || !BB) {
    return false;
  }
  return it->second.body_blocks.count(BB) > 0;
}

std::set<const llvm::BasicBlock *> LoopAbstraction::getLoopHeaders() const {
  std::set<const llvm::BasicBlock *> headers;
  for (const auto &kv : loop_headers_) {
    headers.insert(kv.first);
  }
  return headers;
}

std::optional<ExecutionDomain>
LoopAbstraction::inferInvariant(const llvm::BasicBlock *BB,
                                const ExecutionDomain &entry_state,
                                const ExecutionDomain &current_state) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return std::nullopt;
  }

  // Production-ready invariant inference:
  // 1. Need at least 2 iterations to infer
  // 2. Check if path stamps are converging
  // 3. Compute fixpoint by finding common properties

  if (it->second.iterations < 2) {
    return std::nullopt;
  }

  // Check convergence: if path stamps are the same, we've converged.
  if (hasPreviousIterationSamePathStamp(BB)) {
    ExecutionDomain converged = current_state.clone();
    if (auto *a = converged.getAstate()) {
      a->clearLoopInvariantInference();
    }
    return converged;
  }

  // Attempt PulseJoin-based invariant inference (entry_state ⊔ current_state)
  // with LoopInvariantUnderInference tracking.
  if (auto inv = computeWidenedInvariant(BB, entry_state, current_state)) {
    it->second.candidate_invariant = inv->clone();
    return inv;
  }

  // If we've exceeded max iterations, use entry state as fallback invariant.
  if (it->second.iterations >= kMaxInvariantIterations) {
    ExecutionDomain fallback = entry_state.clone();
    fallback.setEntryPred(current_state.getEntryPred());
    return fallback;
  }

  return std::nullopt;
}

bool LoopAbstraction::isInferringInvariant(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return false;
  }
  // We're inferring if we've visited multiple times but haven't converged
  return it->second.iterations >= 2 &&
         it->second.iterations < kMaxInvariantIterations &&
         !hasPreviousIterationSamePathStamp(BB);
}

void LoopAbstraction::pushLoopInfo(const llvm::BasicBlock *BB,
                                   unsigned timestamp,
                                   const PulseFormula &path_condition) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return;
  }

  PulseFormula path_stamp = path_condition.clone(); // Extract path stamp
  IterationInfo info(timestamp, path_stamp);
  it->second.iteration_stack.push_back(info);

  // Limit stack size to prevent unbounded growth
  if (it->second.iteration_stack.size() > kMaxWidenIterations) {
    it->second.iteration_stack.erase(it->second.iteration_stack.begin());
  }
}

void LoopAbstraction::initLoopInfo(const llvm::BasicBlock *BB) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return;
  }
  it->second.iteration_stack.clear();
  it->second.local_path_condition = PulseFormula(); // Initialize to empty/true
}

void LoopAbstraction::removeLoopInfo(const llvm::BasicBlock *BB) {
  auto it = loop_headers_.find(BB);
  if (it != loop_headers_.end()) {
    it->second.iteration_stack.clear();
  }
}

unsigned LoopAbstraction::getIterationIndex(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return 0;
  }
  return it->second.iteration_stack.size();
}

bool LoopAbstraction::hasPreviousIterationSamePathStamp(
    const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end() || it->second.iteration_stack.size() < 2) {
    return false;
  }

  const auto &stack = it->second.iteration_stack;
  const auto &current = stack.back();
  const auto &previous = stack[stack.size() - 2];
  return current.path_stamp.equivalentTo(previous.path_stamp);
}

bool LoopAbstraction::isCurrentIterationEmptyPathStamp(
    const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end() || it->second.iteration_stack.size() < 2) {
    return false;
  }

  const auto &stack = it->second.iteration_stack;
  return stack[0].path_stamp.isEmptyOrTrivial();
}

void LoopAbstraction::mapFormulas(
    const llvm::BasicBlock *BB,
    std::function<PulseFormula(const PulseFormula &)> f) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return;
  }

  PulseFormula new_condition = f(it->second.local_path_condition);
  it->second.local_path_condition = std::move(new_condition);

  // Also update path stamps in iteration stack
  for (auto &info : it->second.iteration_stack) {
    info.path_stamp = f(info.path_stamp);
  }
}

const ExecutionDomain &
LoopAbstraction::getEntryState(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it != loop_headers_.end()) {
    return it->second.entry_state;
  }
  static ExecutionDomain empty;
  return empty;
}

} // namespace pulse
