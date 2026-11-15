#include "Concurrency/ValueFlow/LockSpanReasoner.h"

#include "Concurrency/LockSet/LockSetAnalysis.h"

#include <utility>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

using namespace llvm;

namespace lotus::analysis {

LockSpanReasoner::LockSpanReasoner(const mhp::LockSetAnalysis &locks)
    : locks_(&locks) {}

LockSpanReasoner::~LockSpanReasoner() = default;

bool LockSpanReasoner::inSameSpan(const Instruction *lhs,
                                  const Instruction *rhs) {
  if (!locks_ || !lhs || !rhs)
    return false;
  if (lhs == rhs)
    return true;

  const bool lhsFirst = std::less<const Instruction *>()(lhs, rhs);
  const auto key =
      lhsFirst ? std::make_pair(lhs, rhs) : std::make_pair(rhs, lhs);
  auto cached = sameSpanCache_.find(key);
  if (cached != sameSpanCache_.end())
    return cached->second;

  const bool sameSpan = computeSameSpan(lhs, rhs);
  sameSpanCache_.emplace(key, sameSpan);
  return sameSpan;
}

bool LockSpanReasoner::computeSameSpan(const Instruction *lhs,
                                       const Instruction *rhs) {
  if (!lhs->getFunction() || lhs->getFunction() != rhs->getFunction())
    return false;

  const std::vector<SpanKey> &lhsKeys = spanKeys(lhs);
  if (lhsKeys.empty())
    return false;
  const std::vector<SpanKey> &rhsKeys = spanKeys(rhs);
  if (rhsKeys.empty())
    return false;

  for (const SpanKey &lhsKey : lhsKeys)
    for (const SpanKey &rhsKey : rhsKeys) {
      if (!(lhsKey == rhsKey))
        continue;
      if (hasInterveningRelease(lhsKey.lock, lhs, rhs))
        continue;
      return true;
    }
  return false;
}

bool LockSpanReasoner::hasInterveningRelease(const Value *lock,
                                             const Instruction *lhs,
                                             const Instruction *rhs) {
  for (const Instruction *release : locks_->getLockReleases(lock)) {
    if (!release || release == lhs || release == rhs)
      continue;
    if (release->getFunction() != lhs->getFunction())
      continue;
    // The lock can only be dropped and re-taken between the two accesses if
    // some release is reachable from the first access and can still reach the
    // second one (that is exactly what the back edge of a loop, or a repeated
    // call into the same worker function, provides).
    if (mayReach(lhs, release) && mayReach(release, rhs))
      return true;
  }
  return false;
}

const std::vector<LockSpanReasoner::SpanKey> &
LockSpanReasoner::spanKeys(const Instruction *inst) {
  auto cached = spanKeyCache_.find(inst);
  if (cached != spanKeyCache_.end())
    return cached->second;

  std::vector<SpanKey> keys;
  if (locks_ && inst && inst->getFunction()) {
    // Only exclusive locks serialize the whole critical section, so only
    // write locks can define a span.
    const mhp::LockSet exclusiveLocks = locks_->getMustWriteLockSetAt(inst);
    for (const mhp::LockID lock : exclusiveLocks) {
      const Instruction *acquire = innermostAcquire(inst, lock);
      if (!acquire)
        continue;
      const Instruction *release = nearestRelease(inst, lock);
      if (!release || release == acquire)
        continue;
      keys.push_back(SpanKey{lock, acquire, release});
    }
  }
  return spanKeyCache_.emplace(inst, std::move(keys)).first->second;
}

const Instruction *LockSpanReasoner::innermostAcquire(const Instruction *inst,
                                                      const Value *lock) {
  DominatorTree *dominators = domTree(inst->getFunction());
  if (!dominators)
    return nullptr;

  const Instruction *innermost = nullptr;
  for (const Instruction *acquire : locks_->getLockAcquires(lock)) {
    if (!acquire || acquire == inst ||
        acquire->getFunction() != inst->getFunction())
      continue;
    if (!dominators->dominates(acquire, inst))
      continue;
    // Acquires dominating `inst` form a chain, so the deepest one is unique.
    if (!innermost || dominators->dominates(innermost, acquire))
      innermost = acquire;
  }
  return innermost;
}

const Instruction *LockSpanReasoner::nearestRelease(const Instruction *inst,
                                                    const Value *lock) {
  const Instruction *nearest = nullptr;
  for (const Instruction *release : locks_->getLockReleases(lock)) {
    if (!release || release == inst ||
        release->getFunction() != inst->getFunction())
      continue;
    if (!postDominates(release, inst))
      continue;
    // Post-dominating releases form a chain, so the closest one is unique.
    if (!nearest || postDominates(nearest, release))
      nearest = release;
  }
  return nearest;
}

bool LockSpanReasoner::postDominates(const Instruction *postDominator,
                                     const Instruction *inst) {
  if (!postDominator || !inst ||
      postDominator->getFunction() != inst->getFunction())
    return false;

  const BasicBlock *postDominatorBlock = postDominator->getParent();
  const BasicBlock *instBlock = inst->getParent();
  if (!postDominatorBlock || !instBlock)
    return false;
  if (postDominatorBlock == instBlock)
    return !postDominator->comesBefore(inst);

  PostDominatorTree *postDominators = postDomTree(inst->getFunction());
  if (!postDominators)
    return false;
  return postDominators->dominates(postDominatorBlock, instBlock);
}

bool LockSpanReasoner::mustExecuteBefore(const Instruction *earlier,
                                         const Instruction *later) {
  if (!earlier || !later || !earlier->getFunction() ||
      earlier->getFunction() != later->getFunction() || earlier == later)
    return false;
  DominatorTree *dominators = domTree(earlier->getFunction());
  if (!dominators)
    return false;
  return dominators->dominates(earlier, later);
}

bool LockSpanReasoner::mustExecuteAfter(const Instruction *earlier,
                                        const Instruction *later) {
  if (!earlier || !later || earlier == later)
    return false;
  return postDominates(later, earlier);
}

bool LockSpanReasoner::mayReach(const Instruction *source,
                                const Instruction *target) {
  if (!source || !target)
    return true;
  const Function *function = source->getFunction();
  if (!function || function != target->getFunction())
    return true;

  const BasicBlock *sourceBlock = source->getParent();
  const BasicBlock *targetBlock = target->getParent();
  if (!sourceBlock || !targetBlock)
    return true;
  if (sourceBlock == targetBlock) {
    if (source == target || source->comesBefore(target))
      return true;
    // A cycle through the block makes the earlier instruction reachable too.
    return reachableFrom(sourceBlock).count(sourceBlock) != 0;
  }
  return reachableFrom(sourceBlock).count(targetBlock) != 0;
}

DominatorTree *LockSpanReasoner::domTree(const Function *func) {
  if (!func)
    return nullptr;
  auto cached = domTrees_.find(func);
  if (cached != domTrees_.end())
    return cached->second.get();

  std::unique_ptr<DominatorTree> tree;
  if (!func->isDeclaration())
    tree = std::make_unique<DominatorTree>(*const_cast<Function *>(func));
  return domTrees_.emplace(func, std::move(tree)).first->second.get();
}

PostDominatorTree *LockSpanReasoner::postDomTree(const Function *func) {
  if (!func || func->isDeclaration())
    return nullptr;
  auto cached = postDomTrees_.find(func);
  if (cached != postDomTrees_.end())
    return cached->second.get();

  std::unique_ptr<PostDominatorTree> tree =
      std::make_unique<PostDominatorTree>(*const_cast<Function *>(func));
  return postDomTrees_.emplace(func, std::move(tree)).first->second.get();
}

const std::unordered_set<const BasicBlock *> &
LockSpanReasoner::reachableFrom(const BasicBlock *block) {
  auto cached = reachabilityCache_.find(block);
  if (cached != reachabilityCache_.end())
    return cached->second;

  std::unordered_set<const BasicBlock *> reachable;
  SmallVector<const BasicBlock *, 16> worklist;
  llvm::append_range(worklist, successors(block));
  while (!worklist.empty()) {
    const BasicBlock *current = worklist.pop_back_val();
    if (!reachable.insert(current).second)
      continue;
    llvm::append_range(worklist, successors(current));
  }
  return reachabilityCache_.emplace(block, std::move(reachable)).first->second;
}

} // namespace lotus::analysis
