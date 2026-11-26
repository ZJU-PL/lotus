
#include "Checker/Pulse/Domain/PulseDisjunctiveDomain.h"

#include "Checker/Pulse/Domain/PulseJoin.h"
#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"

#include <algorithm>

namespace pulse {

static void reduceDisjuncts(std::vector<DisjunctiveDomain::Disjunct> &disjuncts,
                            size_t max,
                            NonDisjunctiveDomain *spill_non_disj = nullptr) {
  if (disjuncts.size() <= max) {
    return;
  }

  auto is_preferred = [](const DisjunctiveDomain::Disjunct &d) {
    auto *a = d.state.getAstate();
    return a && !a->hasUnknownValues();
  };

  std::vector<DisjunctiveDomain::Disjunct> preferred;
  std::vector<DisjunctiveDomain::Disjunct> rest;
  preferred.reserve(disjuncts.size());
  rest.reserve(disjuncts.size());

  for (auto &d : disjuncts) {
    if (is_preferred(d)) {
      preferred.push_back(std::move(d));
    } else {
      rest.push_back(std::move(d));
    }
  }

  std::vector<DisjunctiveDomain::Disjunct> selected;
  selected.reserve(max);
  std::set<const llvm::BasicBlock *> seen_ctx;

  auto pick = [&](std::vector<DisjunctiveDomain::Disjunct> &pool) {
    std::vector<bool> chosen(pool.size(), false);
    for (size_t i = 0; i < pool.size(); ++i) {
      if (selected.size() >= max) {
        break;
      }
      auto &d = pool[i];
      if (!d.path_context) {
        continue;
      }
      if (seen_ctx.insert(d.path_context).second) {
        selected.push_back(std::move(d));
        chosen[i] = true;
      }
    }
    for (size_t i = 0; i < pool.size(); ++i) {
      if (selected.size() >= max) {
        break;
      }
      if (!chosen[i]) {
        selected.push_back(std::move(pool[i]));
        chosen[i] = true;
      }
    }
    if (spill_non_disj) {
      for (size_t i = 0; i < pool.size(); ++i) {
        if (!chosen[i] && pool[i].state.getAstate()) {
          spill_non_disj->addState(*pool[i].state.getAstate());
        }
      }
    }
  };

  pick(preferred);
  pick(rest);

  disjuncts.swap(selected);
}

const std::vector<DisjunctiveDomain::Disjunct> &
DisjunctiveDomain::getDisjuncts(const llvm::BasicBlock *at_block) const {
  static const std::vector<DisjunctiveDomain::Disjunct> kEmpty;
  auto it = disjuncts_by_block_.find(at_block);
  return (it == disjuncts_by_block_.end()) ? kEmpty : it->second;
}

std::vector<DisjunctiveDomain::Disjunct> &
DisjunctiveDomain::getDisjuncts(const llvm::BasicBlock *at_block) {
  return disjuncts_by_block_[at_block];
}

size_t DisjunctiveDomain::size() const {
  size_t total = 0;
  for (const auto &kv : disjuncts_by_block_) {
    total += kv.second.size();
  }
  return total;
}

void DisjunctiveDomain::add(const llvm::BasicBlock *at_block,
                            ExecutionDomain state,
                            const llvm::BasicBlock *path_context) {
  // Preserve predecessor context for sound PHI evaluation.
  state.setEntryPred(path_context);
  disjuncts_by_block_[at_block].emplace_back(std::move(state), path_context);
  limitDisjuncts(at_block);
}

bool DisjunctiveDomain::addWithDedup(const llvm::BasicBlock *at_block,
                                     ExecutionDomain state,
                                     const llvm::BasicBlock *path_context,
                                     NonDisjunctiveDomain *non_disj) {
  state.setEntryPred(path_context);
  auto &vec = disjuncts_by_block_[at_block];
  if (const AbductiveDomain *cand = state.getAstate()) {
    for (const auto &existing : vec) {
      if (existing.path_context == path_context && existing.state.getAstate() &&
          AbductiveDomain::leq(*cand, *existing.state.getAstate())) {
        return false;
      }
    }
  }
  vec.emplace_back(std::move(state), path_context);
  reduceDisjuncts(vec, kMaxDisjuncts, non_disj);
  return true;
}

void DisjunctiveDomain::limitDisjuncts(const llvm::BasicBlock *at_block) {
  auto it = disjuncts_by_block_.find(at_block);
  if (it == disjuncts_by_block_.end()) {
    return;
  }
  reduceDisjuncts(it->second, kMaxDisjuncts);
}

ExecutionDomain DisjunctiveDomain::joinAtBlock(const llvm::BasicBlock *BB) {
  auto it = disjuncts_by_block_.find(BB);
  if (it == disjuncts_by_block_.end() || it->second.empty()) {
    return ExecutionDomain();
  }

  auto &disjuncts = it->second;
  if (disjuncts.size() == 1) {
    return disjuncts[0].state.clone();
  }

  // Group non-stopped disjuncts by CFG predecessor (path_context) so PHI
  // evaluation remains sound, then join same-predecessor disjuncts via
  // PulseJoin::joinAbductive (preserving shared facts and disjunctive
  // equalities), falling back to a clean representative if join is unavailable.
  const DisjunctiveDomain::Disjunct *best = nullptr;
  for (const auto &disj : disjuncts) {
    if (disj.state.isStopped()) {
      continue;
    }
    if (!best) {
      best = &disj;
      continue;
    }
    auto *a_best = best->state.getAstate();
    auto *a_cur = disj.state.getAstate();
    const bool best_unknown = a_best ? a_best->hasUnknownValues() : true;
    const bool cur_unknown = a_cur ? a_cur->hasUnknownValues() : true;
    if (best_unknown && !cur_unknown) {
      best = &disj;
    }
  }
  if (best) {
    const llvm::BasicBlock *target_pred = best->path_context;
    if (const AbductiveDomain *acc_init = best->state.getAstate()) {
      AbductiveDomain acc = acc_init->clone();
      bool joined_any = false;
      for (const auto &disj : disjuncts) {
        if (&disj == best || disj.state.isStopped() ||
            disj.path_context != target_pred || !disj.state.getAstate()) {
          continue;
        }
        if (auto merged = PulseJoin::joinAbductive(acc, *disj.state.getAstate())) {
          acc = std::move(*merged);
          joined_any = true;
        }
      }
      if (joined_any) {
        ExecutionDomain joined = ExecutionDomain::continueProgram(
            std::make_unique<AbductiveDomain>(std::move(acc)));
        joined.setEntryPred(target_pred);
        return joined;
      }
    }
    return best->state.clone();
  }

  ExecutionDomain stopped;
  stopped.setState(ExecutionState::Stopped);
  return stopped;
}

bool DisjunctiveDomain::shouldWiden(const llvm::BasicBlock *BB) const {
  auto it = block_iterations_.find(BB);
  if (it == block_iterations_.end()) {
    return false;
  }
  return it->second >= kWidenThreshold;
}

void DisjunctiveDomain::widen(const llvm::BasicBlock *BB) {
  auto it = block_iterations_.find(BB);
  if (it == block_iterations_.end()) {
    block_iterations_[BB] = 1;
  } else {
    it->second++;
  }

  // Apply widening if threshold reached
  if (block_iterations_[BB] >= kWidenThreshold) {
    auto it = disjuncts_by_block_.find(BB);
    if (it != disjuncts_by_block_.end()) {
      reduceDisjuncts(it->second, kWidenKeepDisjuncts);
    }
  }
}

} // namespace pulse
