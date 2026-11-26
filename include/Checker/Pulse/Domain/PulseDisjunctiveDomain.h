/** @file PulseDisjunctiveDomain.h @brief Disjunctive abstract domain for Pulse analysis. */
#pragma once

#include "Checker/Pulse/Domain/PulseDomain.h"

#include <map>
#include <set>
#include <vector>

#include <llvm/IR/BasicBlock.h>

namespace llvm {
class BasicBlock;
} // namespace llvm

namespace pulse {

class NonDisjunctiveDomain;

/**
 * DisjunctiveDomain: manages multiple disjunctive states
 * Implements proper disjunctive abstract domain with widening
 */
class DisjunctiveDomain {
public:
  // A disjunct is a pair of (execution state, path context)
  // For now, path context is just the basic block
  struct Disjunct {
    ExecutionDomain state;
    const llvm::BasicBlock *path_context;

    Disjunct() : path_context(nullptr) {}

    Disjunct(ExecutionDomain s, const llvm::BasicBlock *ctx)
        : state(std::move(s)), path_context(ctx) {}
  };

private:
  // Disjuncts are tracked per basic block. This matters: joining/widening
  // should only combine states that are at the same program point (block
  // entry).
  std::map<const llvm::BasicBlock *, std::vector<Disjunct>> disjuncts_by_block_;
  static constexpr size_t kMaxDisjuncts = 10;
  static constexpr unsigned kWidenThreshold = 3; // Widen after 3 iterations
  static constexpr unsigned kMaxBlockVisits = 8; // Cap total joins across huge switches
  static constexpr size_t kWidenKeepDisjuncts = 4;

  // Track iterations per block and per (block, pred) edge for widening
  std::map<const llvm::BasicBlock *, unsigned> block_iterations_;
  std::map<std::pair<const llvm::BasicBlock *, const llvm::BasicBlock *>,
           unsigned>
      edge_iterations_;
  std::set<std::pair<const llvm::BasicBlock *, const llvm::BasicBlock *>>
      widened_edges_;

public:
  DisjunctiveDomain() = default;

  /**
   * Add a disjunct
   */
  void add(const llvm::BasicBlock *at_block, ExecutionDomain state,
           const llvm::BasicBlock *path_context);

  /**
   * Add a disjunct with graph-isomorphism deduplication (`AbductiveDomain::leq`)
   * and optional spilling of dropped disjuncts into a NonDisjunctiveDomain.
   * Returns false if an isomorphic disjunct was already present for the same
   * predecessor context.
   */
  bool addWithDedup(const llvm::BasicBlock *at_block, ExecutionDomain state,
                    const llvm::BasicBlock *path_context,
                    NonDisjunctiveDomain *non_disj = nullptr);
  bool addWithDedupRef(const llvm::BasicBlock *at_block,
                       const ExecutionDomain &state,
                       const llvm::BasicBlock *path_context,
                       NonDisjunctiveDomain *non_disj = nullptr);

  /**
   * Get disjuncts recorded at a given block
   */
  const std::vector<Disjunct> &
  getDisjuncts(const llvm::BasicBlock *at_block) const;
  std::vector<Disjunct> &getDisjuncts(const llvm::BasicBlock *at_block);

  /**
   * Get total number of disjuncts across all blocks
   */
  size_t size() const;

  /**
   * Join disjuncts at a block entry
   * Returns the joined state (or first state if only one)
   */
  ExecutionDomain joinAtBlock(const llvm::BasicBlock *BB);
  ExecutionDomain joinAtBlockForPred(const llvm::BasicBlock *BB,
                                     const llvm::BasicBlock *pred_bb);

  /**
   * Widen: apply widening operator if needed
   */
  void widen(const llvm::BasicBlock *BB);
  void recordEdgeVisit(const llvm::BasicBlock *BB,
                       const llvm::BasicBlock *pred_bb);
  bool shouldWidenEdge(const llvm::BasicBlock *BB,
                       const llvm::BasicBlock *pred_bb) const;
  bool hasWidenedEdge(const llvm::BasicBlock *BB,
                      const llvm::BasicBlock *pred_bb) const;
  void markWidenedEdge(const llvm::BasicBlock *BB,
                       const llvm::BasicBlock *pred_bb);

  /**
   * Check if we should widen at this block
   */
  bool shouldWiden(const llvm::BasicBlock *BB) const;
  bool hasReachedBlockVisitCap(const llvm::BasicBlock *BB) const {
    auto it = block_iterations_.find(BB);
    return it != block_iterations_.end() && it->second > kMaxBlockVisits;
  }

  /**
   * Clear disjuncts
   */
  void clear() {
    disjuncts_by_block_.clear();
    block_iterations_.clear();
    edge_iterations_.clear();
    widened_edges_.clear();
  }

  /**
   * Check if empty
   */
  bool empty() const { return disjuncts_by_block_.empty(); }

  /**
   * Limit disjuncts to max
   */
  void limitDisjuncts(const llvm::BasicBlock *at_block);
};

} // namespace pulse

