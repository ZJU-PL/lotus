/**
 * @file LockSpanReasoner.h
 * @brief Critical-section ("lock span") identity reasoning for thread value
 *        flow.
 */
#pragma once

#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm {
class BasicBlock;
class DominatorTree;
class Function;
class Instruction;
class PostDominatorTree;
class Value;
} // namespace llvm

namespace mhp {
class LockSetAnalysis;
} // namespace mhp

namespace lotus::analysis {

/// Identifies the critical-section instance ("lock span") a memory access
/// executes in.
///
/// Two accesses are considered to be in the same span when all of the following
/// hold:
///
///  - some lock `L` is held in *exclusive* mode (must-write) at both accesses,
///  - the innermost acquire instruction of `L` that dominates each access is
///    one and the same instruction,
///  - the nearest release instruction of `L` that post-dominates each access is
///    one and the same instruction, and
///  - no release of `L` can be executed in between, so the lock is never
///    dropped and re-taken between the two accesses.
///
/// Keying on the acquire/release *instruction pair* rather than on the lock
/// value alone is what keeps two iterations of a loop, and two threads running
/// the same worker function, in different spans: in both cases the pair is
/// reused, so the "no release in between" test fails (CFG reachability crosses
/// the back edge / the enclosing call) and the query answers conservatively
/// `false`.
///
/// The reasoner caches dominator trees, post-dominator trees, CFG reachability,
/// and per-instruction span keys, so repeated queries are cheap.
class LockSpanReasoner {
public:
  explicit LockSpanReasoner(const mhp::LockSetAnalysis &locks);
  ~LockSpanReasoner();

  LockSpanReasoner(const LockSpanReasoner &) = delete;
  LockSpanReasoner &operator=(const LockSpanReasoner &) = delete;

  /// True when both accesses run inside one and the same critical section of a
  /// common exclusive lock.
  bool inSameSpan(const llvm::Instruction *lhs, const llvm::Instruction *rhs);

  /// True when `earlier` dominates `later`, i.e. every path to `later` executes
  /// `earlier` first. False for inter-procedural or unknown inputs.
  bool mustExecuteBefore(const llvm::Instruction *earlier,
                         const llvm::Instruction *later);

  /// True when `later` post-dominates `earlier`, i.e. every path leaving
  /// `earlier` executes `later`. False for inter-procedural or unknown inputs.
  bool mustExecuteAfter(const llvm::Instruction *earlier,
                        const llvm::Instruction *later);

  /// True when some path in the CFG may reach `target` from `source`.
  /// Inter-procedural and unknown queries answer `true`.
  bool mayReach(const llvm::Instruction *source,
                const llvm::Instruction *target);

private:
  struct SpanKey {
    const llvm::Value *lock = nullptr;
    const llvm::Instruction *acquire = nullptr;
    const llvm::Instruction *release = nullptr;

    bool operator==(const SpanKey &other) const {
      return lock == other.lock && acquire == other.acquire &&
             release == other.release;
    }
  };

  const std::vector<SpanKey> &spanKeys(const llvm::Instruction *inst);
  bool computeSameSpan(const llvm::Instruction *lhs,
                       const llvm::Instruction *rhs);
  bool hasInterveningRelease(const llvm::Value *lock,
                             const llvm::Instruction *lhs,
                             const llvm::Instruction *rhs);
  const llvm::Instruction *innermostAcquire(const llvm::Instruction *inst,
                                            const llvm::Value *lock);
  const llvm::Instruction *nearestRelease(const llvm::Instruction *inst,
                                          const llvm::Value *lock);
  bool postDominates(const llvm::Instruction *postDominator,
                     const llvm::Instruction *inst);

  llvm::DominatorTree *domTree(const llvm::Function *func);
  llvm::PostDominatorTree *postDomTree(const llvm::Function *func);
  const std::unordered_set<const llvm::BasicBlock *> &
  reachableFrom(const llvm::BasicBlock *block);

  const mhp::LockSetAnalysis *locks_;
  std::unordered_map<const llvm::Function *,
                     std::unique_ptr<llvm::DominatorTree>>
      domTrees_;
  std::unordered_map<const llvm::Function *,
                     std::unique_ptr<llvm::PostDominatorTree>>
      postDomTrees_;
  std::unordered_map<const llvm::Instruction *, std::vector<SpanKey>>
      spanKeyCache_;
  std::unordered_map<const llvm::BasicBlock *,
                     std::unordered_set<const llvm::BasicBlock *>>
      reachabilityCache_;
  std::map<std::pair<const llvm::Instruction *, const llvm::Instruction *>,
           bool>
      sameSpanCache_;
};

} // namespace lotus::analysis
