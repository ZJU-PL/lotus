#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Store.h"

#include <algorithm>
#include <array>

#include <llvm/ADT/SmallVector.h>

namespace tpa {

namespace {
struct SubsumedCacheEntry {
  uint64_t aVer = 0;
  uint64_t bVer = 0;
};
constexpr size_t kSubsumedCacheSize = 16384;
thread_local std::array<SubsumedCacheEntry, kSubsumedCacheSize>
    subsumedCache{};

inline size_t subsumedSlot(uint64_t aVer, uint64_t bVer) noexcept {
  return ((aVer * 11400714819323198485ULL) ^ (bVer * 14029467366897019727ULL)) &
         (kSubsumedCacheSize - 1u);
}
} // namespace

Store::LeafNode::LeafNode(const LeafNode &other) : Node(0) {
  presentMask = other.presentMask;
  nonAccChildMask = other.nonAccChildMask;
  accToNonAccMask = other.accToNonAccMask;
  totalCount = other.totalCount;
  nonAccCount = other.nonAccCount;
  for (unsigned i = 0; i < kFanout; ++i) {
    keys[i] = other.keys[i];
    vals[i] = other.vals[i];
  }
}

Store::BranchNode::BranchNode(const BranchNode &other) : Node(other.level) {
  presentMask = other.presentMask;
  nonAccChildMask = other.nonAccChildMask;
  accToNonAccMask = other.accToNonAccMask;
  totalCount = other.totalCount;
  nonAccCount = other.nonAccCount;
  uint16_t mask = presentMask;
  while (mask) {
    unsigned s = static_cast<unsigned>(__builtin_ctz(mask));
    mask &= static_cast<uint16_t>(mask - 1u);
    children[s] = retain(other.children[s]);
  }
}

void Store::release(const Node *n) noexcept {
  if (n && n->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1u)
    destroyNode(n);
}

void Store::destroyNode(const Node *n) noexcept {
  if (n->level == 0) {
    delete static_cast<const LeafNode *>(n);
    return;
  }
  const auto *branch = static_cast<const BranchNode *>(n);
  uint16_t mask = branch->presentMask;
  while (mask) {
    unsigned s = static_cast<unsigned>(__builtin_ctz(mask));
    mask &= static_cast<uint16_t>(mask - 1u);
    release(branch->children[s]);
  }
  delete branch;
}

static inline void refreshBranchSlot(Store::BranchNode *branch, unsigned slot,
                                     const Store::Node *oldChild,
                                     const Store::Node *newChild) noexcept {
  const uint16_t bit = static_cast<uint16_t>(1u << slot);
  if (oldChild) {
    branch->totalCount -= oldChild->totalCount;
    branch->nonAccCount -= oldChild->nonAccCount;
  }
  if (newChild) {
    branch->children[slot] = newChild;
    branch->presentMask |= bit;
    branch->totalCount += newChild->totalCount;
    branch->nonAccCount += newChild->nonAccCount;
    if (newChild->nonAccCount != 0)
      branch->nonAccChildMask |= bit;
    else
      branch->nonAccChildMask &= static_cast<uint16_t>(~bit);
    if (newChild->accToNonAccMask != 0)
      branch->accToNonAccMask |= bit;
    else
      branch->accToNonAccMask &= static_cast<uint16_t>(~bit);
  } else {
    branch->children[slot] = nullptr;
    branch->presentMask &= static_cast<uint16_t>(~bit);
    branch->nonAccChildMask &= static_cast<uint16_t>(~bit);
    branch->accToNonAccMask &= static_cast<uint16_t>(~bit);
  }
}

static inline void updateLeafSlotMetadata(Store::LeafNode *leaf, unsigned slot,
                                          const MemoryObject *key, PtsSet val,
                                          bool wasPresent) noexcept {
  const uint16_t bit = static_cast<uint16_t>(1u << slot);
  const bool accessible = !(key->isStackObject() || key->isHeapObject());
  if (!wasPresent) {
    leaf->keys[slot] = key;
    leaf->presentMask |= bit;
    ++leaf->totalCount;
    if (!accessible) {
      leaf->nonAccChildMask |= bit;
      ++leaf->nonAccCount;
    }
  }
  leaf->vals[slot] = val;
  if (accessible && val.hasNonAccessible())
    leaf->accToNonAccMask |= bit;
  else
    leaf->accToNonAccMask &= static_cast<uint16_t>(~bit);
}

void Store::ensureCapacity(uint32_t id) {
  if (!root) {
    uint8_t lvl = 0;
    while (id >= capacityForLevel(lvl))
      ++lvl;
    if (lvl == 0)
      root = new LeafNode();
    else
      root = new BranchNode(lvl);
    return;
  }
  while (id >= capacityForLevel(root->level)) {
    auto *parent = new BranchNode(static_cast<uint8_t>(root->level + 1u));
    refreshBranchSlot(parent, 0u, nullptr, root);
    root = parent;
  }
}

const PtsSet *Store::findOwnBinding(const MemoryObject *key) const {
  assert(key != nullptr);
  if (!root)
    return nullptr;
  const uint32_t id = key->getId();
  if (id >= capacityForLevel(root->level))
    return nullptr;

  const Node *cur = root;
  while (cur->level > 0) {
    const unsigned slot = (id >> (static_cast<unsigned>(cur->level) * kBits)) & kMask;
    const auto *branch = static_cast<const BranchNode *>(cur);
    cur = branch->children[slot];
    if (!cur)
      return nullptr;
  }
  const unsigned slot = id & kMask;
  const auto *leaf = static_cast<const LeafNode *>(cur);
  if (!(leaf->presentMask & (1u << slot)))
    return nullptr;
  assert(leaf->keys[slot] == key);
  return &leaf->vals[slot];
}

const Store::Node *Store::updateNode(const Node *node, uint8_t level,
                                     uint32_t id, const MemoryObject *key,
                                     PtsSet value, bool isStrong,
                                     bool canMutate, bool &changed) {
  const bool selfUnique =
      node != nullptr && canMutate &&
      node->refCount.load(std::memory_order_acquire) == 1u;

  if (level == 0) {
    const unsigned slot = id & kMask;
    const uint16_t bit = static_cast<uint16_t>(1u << slot);
    if (!node) {
      auto *leaf = new LeafNode();
      updateLeafSlotMetadata(leaf, slot, key, value, false);
      changed = true;
      return leaf;
    }
    const auto *leaf = static_cast<const LeafNode *>(node);
    const bool present = (leaf->presentMask & bit) != 0;
    PtsSet nextVal = value;
    if (present) {
      assert(leaf->keys[slot] == key);
      const PtsSet existing = leaf->vals[slot];
      if (!isStrong) {
        if (existing == value) {
          changed = false;
          return node;
        }
        nextVal = existing.merge(value);
      }
      if (nextVal == existing) {
        changed = false;
        return node;
      }
    }
    changed = true;
    LeafNode *target =
        selfUnique ? const_cast<LeafNode *>(leaf) : new LeafNode(*leaf);
    if (selfUnique)
      target->versionId = allocVersionId();
    updateLeafSlotMetadata(target, slot, key, nextVal, present);
    return target;
  }

  const unsigned slot = (id >> (static_cast<unsigned>(level) * kBits)) & kMask;
  if (!node) {
    const Node *child = updateNode(nullptr, static_cast<uint8_t>(level - 1u),
                                   id, key, value, isStrong, true, changed);
    auto *branch = new BranchNode(level);
    refreshBranchSlot(branch, slot, nullptr, child);
    return branch;
  }

  const auto *branch = static_cast<const BranchNode *>(node);
  const Node *oldChild = branch->children[slot];
  const uint32_t oldTotal = oldChild ? oldChild->totalCount : 0u;
  const uint32_t oldNonAcc = oldChild ? oldChild->nonAccCount : 0u;

  const Node *newChild =
      updateNode(oldChild, static_cast<uint8_t>(level - 1u), id, key, value,
                 isStrong, selfUnique, changed);
  if (!changed)
    return node;

  BranchNode *target =
      selfUnique ? const_cast<BranchNode *>(branch) : new BranchNode(*branch);
  if (selfUnique)
    target->versionId = allocVersionId();
  if (newChild != oldChild) {
    // If we cloned `branch`, the clone retained `oldChild`; if `selfUnique`,
    // `branch` owned `oldChild`. In both cases release `oldChild` once.
    release(oldChild);
    target->children[slot] = newChild;
  }
  const uint16_t bit = static_cast<uint16_t>(1u << slot);
  target->presentMask |= bit;
  target->totalCount = target->totalCount - oldTotal + newChild->totalCount;
  target->nonAccCount = target->nonAccCount - oldNonAcc + newChild->nonAccCount;
  if (newChild->nonAccCount != 0)
    target->nonAccChildMask |= bit;
  else
    target->nonAccChildMask &= static_cast<uint16_t>(~bit);
  if (newChild->accToNonAccMask != 0)
    target->accToNonAccMask |= bit;
  else
    target->accToNonAccMask &= static_cast<uint16_t>(~bit);
  return target;
}

bool Store::insert(const MemoryObject *key, const MemoryObject *obj) {
  assert(key != nullptr && obj != nullptr);
  return weakUpdate(key, PtsSet::getSingletonSet(obj));
}

bool Store::weakUpdate(const MemoryObject *key, PtsSet pSet) {
  assert(key != nullptr);
  const uint32_t id = key->getId();
  ensureCapacity(id);
  bool changed = false;
  const Node *newRoot =
      updateNode(root, root->level, id, key, pSet, false, true, changed);
  if (newRoot != root) {
    release(root);
    root = newRoot;
  }
  return changed;
}

bool Store::strongUpdate(const MemoryObject *key, PtsSet pSet) {
  assert(key != nullptr);
  const uint32_t id = key->getId();
  ensureCapacity(id);
  bool changed = false;
  const Node *newRoot =
      updateNode(root, root->level, id, key, pSet, true, true, changed);
  if (newRoot != root) {
    release(root);
    root = newRoot;
  }
  return changed;
}

const Store::Node *Store::mergeSameLevel(const Node *a, const Node *b,
                                         bool canMutateA, bool &changed) {
  if (a == b || b == nullptr) {
    changed = false;
    return a;
  }
  if (a == nullptr) {
    changed = true;
    return retain(b);
  }
  assert(a->level == b->level);
  const uint64_t aVer = a->versionId;
  const uint64_t bVer = b->versionId;
  const size_t cacheIdx = subsumedSlot(aVer, bVer);
  if (subsumedCache[cacheIdx].aVer == aVer &&
      subsumedCache[cacheIdx].bVer == bVer) {
    changed = false;
    return a;
  }

  const bool selfUnique =
      canMutateA && a->refCount.load(std::memory_order_acquire) == 1u;

  if (a->level == 0) {
    const auto *la = static_cast<const LeafNode *>(a);
    const auto *lb = static_cast<const LeafNode *>(b);

    // First pass: check whether `b` actually changes any slot of `a`, and
    // whether `b` already subsumes `a` (so we can structurally share `b`).
    bool anyChange = false;
    bool bSubsumesA = (lb->presentMask & la->presentMask) == la->presentMask;
    PtsSet mergedVals[kFanout];
    uint16_t changedMask = 0;

    uint16_t bMask = lb->presentMask;
    while (bMask) {
      unsigned s = static_cast<unsigned>(__builtin_ctz(bMask));
      bMask &= static_cast<uint16_t>(bMask - 1u);
      const uint16_t bit = static_cast<uint16_t>(1u << s);
      if (!(la->presentMask & bit)) {
        anyChange = true;
        changedMask |= bit;
        mergedVals[s] = lb->vals[s];
      } else {
        const PtsSet va = la->vals[s];
        const PtsSet vb = lb->vals[s];
        if (va == vb)
          continue;
        const PtsSet merged = va.merge(vb);
        if (merged != va) {
          anyChange = true;
          changedMask |= bit;
          mergedVals[s] = merged;
        }
        if (merged != vb)
          bSubsumesA = false;
      }
    }

    if (!anyChange) {
      subsumedCache[cacheIdx] = {aVer, bVer};
      changed = false;
      return a;
    }
    changed = true;
    if (bSubsumesA)
      return retain(b);

    LeafNode *target =
        selfUnique ? const_cast<LeafNode *>(la) : new LeafNode(*la);
    if (selfUnique)
      target->versionId = allocVersionId();
    while (changedMask) {
      unsigned s = static_cast<unsigned>(__builtin_ctz(changedMask));
      changedMask &= static_cast<uint16_t>(changedMask - 1u);
      const uint16_t bit = static_cast<uint16_t>(1u << s);
      const bool wasPresent = (target->presentMask & bit) != 0;
      updateLeafSlotMetadata(target, s, lb->keys[s], mergedVals[s], wasPresent);
    }
    return target;
  }

  const auto *ba = static_cast<const BranchNode *>(a);
  const auto *bb = static_cast<const BranchNode *>(b);
  bool anyChange = false;
  bool bSubsumesA = (bb->presentMask & ba->presentMask) == ba->presentMask;
  const Node *newChildren[kFanout];
  uint32_t oldTotals[kFanout];
  uint32_t oldNonAccs[kFanout];
  uint16_t changedMask = 0;

  uint16_t bMask = bb->presentMask;
  while (bMask) {
    unsigned s = static_cast<unsigned>(__builtin_ctz(bMask));
    bMask &= static_cast<uint16_t>(bMask - 1u);

    const Node *oldChild = ba->children[s];
    const Node *bChild = bb->children[s];
    if (oldChild == bChild)
      continue;

    oldTotals[s] = oldChild ? oldChild->totalCount : 0u;
    oldNonAccs[s] = oldChild ? oldChild->nonAccCount : 0u;

    bool childChanged = false;
    const Node *newChild =
        mergeSameLevel(oldChild, bChild, selfUnique, childChanged);
    if (newChild != bChild)
      bSubsumesA = false;
    if (!childChanged)
      continue;

    anyChange = true;
    changedMask |= static_cast<uint16_t>(1u << s);
    newChildren[s] = newChild;
  }

  if (!anyChange) {
    subsumedCache[cacheIdx] = {aVer, bVer};
    changed = false;
    return a;
  }
  changed = true;
  if (bSubsumesA) {
    // Every changed slot returned retain(bChild) without mutating ba's children.
    while (changedMask) {
      unsigned s = static_cast<unsigned>(__builtin_ctz(changedMask));
      changedMask &= static_cast<uint16_t>(changedMask - 1u);
      release(newChildren[s]);
    }
    return retain(b);
  }

  BranchNode *target =
      selfUnique ? const_cast<BranchNode *>(ba) : new BranchNode(*ba);
  if (selfUnique)
    target->versionId = allocVersionId();
  while (changedMask) {
    unsigned s = static_cast<unsigned>(__builtin_ctz(changedMask));
    changedMask &= static_cast<uint16_t>(changedMask - 1u);
    const Node *oldChild = target->children[s];
    const Node *newChild = newChildren[s];
    if (newChild != oldChild) {
      release(oldChild);
      target->children[s] = newChild;
    }
    const uint16_t bit = static_cast<uint16_t>(1u << s);
    target->presentMask |= bit;
    target->totalCount = target->totalCount - oldTotals[s] + newChild->totalCount;
    target->nonAccCount =
        target->nonAccCount - oldNonAccs[s] + newChild->nonAccCount;
    if (newChild->nonAccCount != 0)
      target->nonAccChildMask |= bit;
    else
      target->nonAccChildMask &= static_cast<uint16_t>(~bit);
    if (newChild->accToNonAccMask != 0)
      target->accToNonAccMask |= bit;
    else
      target->accToNonAccMask &= static_cast<uint16_t>(~bit);
  }

  return target;
}

const Store::Node *Store::mergeLowerIntoHigher(const Node *a, const Node *b,
                                               bool canMutateA, bool &changed) {
  assert(a != nullptr && b != nullptr && a->level >= b->level);
  if (a->level == b->level)
    return mergeSameLevel(a, b, canMutateA, changed);

  const bool selfUnique =
      canMutateA && a->refCount.load(std::memory_order_acquire) == 1u;
  const auto *ba = static_cast<const BranchNode *>(a);
  const Node *oldChild = ba->children[0];
  const uint32_t oldTotal = oldChild ? oldChild->totalCount : 0u;
  const uint32_t oldNonAcc = oldChild ? oldChild->nonAccCount : 0u;

  const Node *newChild = nullptr;
  if (!oldChild) {
    // Wrap `b` up to `a->level - 1`.
    const Node *wrapped = retain(b);
    while (wrapped->level + 1u < a->level) {
      auto *parent = new BranchNode(static_cast<uint8_t>(wrapped->level + 1u));
      refreshBranchSlot(parent, 0u, nullptr, wrapped);
      wrapped = parent;
    }
    newChild = wrapped;
    changed = true;
  } else {
    newChild = mergeLowerIntoHigher(oldChild, b, selfUnique, changed);
  }
  if (!changed)
    return a;

  BranchNode *target =
      selfUnique ? const_cast<BranchNode *>(ba) : new BranchNode(*ba);
  if (selfUnique)
    target->versionId = allocVersionId();
  if (newChild != oldChild) {
    release(oldChild);
    target->children[0] = newChild;
  }
  target->presentMask |= 1u;
  target->totalCount = target->totalCount - oldTotal + newChild->totalCount;
  target->nonAccCount = target->nonAccCount - oldNonAcc + newChild->nonAccCount;
  if (newChild->nonAccCount != 0)
    target->nonAccChildMask |= 1u;
  else
    target->nonAccChildMask &= static_cast<uint16_t>(~1u);
  if (newChild->accToNonAccMask != 0)
    target->accToNonAccMask |= 1u;
  else
    target->accToNonAccMask &= static_cast<uint16_t>(~1u);
  return target;
}

bool Store::mergeWith(const Store &rhs) {
  if (root == rhs.root || rhs.root == nullptr)
    return false;
  if (root == nullptr) {
    root = retain(rhs.root);
    return true;
  }
  while (root->level < rhs.root->level) {
    auto *parent = new BranchNode(static_cast<uint8_t>(root->level + 1u));
    refreshBranchSlot(parent, 0u, nullptr, root);
    root = parent;
  }
  bool changed = false;
  const Node *newRoot = mergeLowerIntoHigher(root, rhs.root, true, changed);
  if (newRoot != root) {
    release(root);
    root = newRoot;
  }
  return changed;
}

const Store::Node *Store::filterNodeRange(const Node *node, uint64_t baseId,
                                          const uint32_t *lo,
                                          const uint32_t *hi) {
  if (!node)
    return nullptr;
  // Pure-global subtrees require zero work and remain 100% shared.
  if (node->nonAccCount == 0)
    return retain(node);
  // Pure non-accessible subtrees with no reachable IDs in their ID interval
  // drop in O(1) without descending a single level.
  if (lo == hi && node->nonAccCount == node->totalCount)
    return nullptr;

  if (node->level == 0) {
    const auto *leaf = static_cast<const LeafNode *>(node);
    uint16_t reachMask = 0;
    for (const uint32_t *p = lo; p != hi; ++p)
      reachMask |= static_cast<uint16_t>(1u << (*p & kMask));
    const uint16_t dropMask =
        static_cast<uint16_t>(leaf->nonAccChildMask & ~reachMask);
    if (dropMask == 0)
      return retain(leaf);
    const uint16_t keepMask =
        static_cast<uint16_t>(leaf->presentMask & ~dropMask);
    if (keepMask == 0)
      return nullptr;
    auto *filtered = new LeafNode();
    uint16_t m = keepMask;
    while (m) {
      unsigned s = static_cast<unsigned>(__builtin_ctz(m));
      m &= static_cast<uint16_t>(m - 1u);
      updateLeafSlotMetadata(filtered, s, leaf->keys[s], leaf->vals[s], false);
    }
    return filtered;
  }

  const auto *branch = static_cast<const BranchNode *>(node);
  const unsigned shift = static_cast<unsigned>(branch->level) * kBits;
  const uint64_t step = 1ULL << shift;
  const Node *newChildren[kFanout];
  bool anyChange = false;
  const uint32_t *cur = lo;

  uint16_t m = branch->presentMask;
  while (m) {
    unsigned s = static_cast<unsigned>(__builtin_ctz(m));
    m &= static_cast<uint16_t>(m - 1u);
    const uint16_t bit = static_cast<uint16_t>(1u << s);
    const Node *oldChild = branch->children[s];
    if (!(branch->nonAccChildMask & bit)) {
      newChildren[s] = retain(oldChild);
      continue;
    }

    const uint64_t slotLo = baseId + (static_cast<uint64_t>(s) << shift);
    const uint64_t slotHi = slotLo + step;
    if (cur != hi && static_cast<uint64_t>(*cur) < slotLo) {
      cur = std::lower_bound(cur, hi, static_cast<uint32_t>(slotLo));
    }
    const uint32_t *subLo = cur;
    const uint32_t *subHi = cur;
    if (cur != hi && static_cast<uint64_t>(*cur) < slotHi) {
      if (slotHi >= (1ULL << 32) || s == kFanout - 1u) {
        subHi = hi;
      } else {
        subHi = std::lower_bound(cur, hi, static_cast<uint32_t>(slotHi));
      }
      cur = subHi;
    }

    if (subLo == subHi && oldChild->nonAccCount == oldChild->totalCount) {
      newChildren[s] = nullptr;
      anyChange = true;
    } else {
      newChildren[s] = filterNodeRange(oldChild, slotLo, subLo, subHi);
      if (newChildren[s] != oldChild)
        anyChange = true;
    }
  }
  if (!anyChange) {
    m = branch->presentMask;
    while (m) {
      unsigned s = static_cast<unsigned>(__builtin_ctz(m));
      m &= static_cast<uint16_t>(m - 1u);
      release(newChildren[s]);
    }
    return retain(branch);
  }

  auto *filtered = new BranchNode(branch->level);
  m = branch->presentMask;
  while (m) {
    unsigned s = static_cast<unsigned>(__builtin_ctz(m));
    m &= static_cast<uint16_t>(m - 1u);
    if (newChildren[s])
      refreshBranchSlot(filtered, s, nullptr, newChildren[s]);
  }
  if (filtered->presentMask == 0) {
    delete filtered;
    return nullptr;
  }
  return filtered;
}

Store Store::filterNonAccessibleIds(llvm::ArrayRef<uint32_t> sortedIds) const {
  if (!root || root->nonAccCount == 0)
    return *this;
  const uint32_t *lo = sortedIds.begin();
  const uint32_t *hi = sortedIds.end();
  const uint64_t cap = capacityForLevel(root->level);
  if (cap < (1ULL << 32))
    hi = std::lower_bound(lo, hi, static_cast<uint32_t>(cap));
  Store result;
  result.root = filterNodeRange(root, 0ULL, lo, hi);
  while (result.root && result.root->level > 0 &&
         result.root->presentMask == 1u) {
    const auto *branch = static_cast<const BranchNode *>(result.root);
    const Node *onlyChild = retain(branch->children[0]);
    release(result.root);
    result.root = onlyChild;
  }
  return result;
}

Store Store::filterNonAccessible(
    const llvm::DenseSet<const MemoryObject *> &reachableNonAcc) const {
  if (!root || root->nonAccCount == 0)
    return *this;
  llvm::SmallVector<uint32_t, 64> ids;
  ids.reserve(reachableNonAcc.size());
  for (const auto *obj : reachableNonAcc)
    if (obj)
      ids.push_back(obj->getId());
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return filterNonAccessibleIds(ids);
}

bool Store::nodesEqual(const Node *a, const Node *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  if (a->totalCount != b->totalCount || a->nonAccCount != b->nonAccCount)
    return false;
  while (a->level > b->level) {
    if (a->presentMask != 1u)
      return false;
    a = static_cast<const BranchNode *>(a)->children[0];
    if (!a)
      return false;
  }
  while (b->level > a->level) {
    if (b->presentMask != 1u)
      return false;
    b = static_cast<const BranchNode *>(b)->children[0];
    if (!b)
      return false;
  }
  if (a == b)
    return true;
  if (a->presentMask != b->presentMask)
    return false;
  if (a->level == 0) {
    const auto *la = static_cast<const LeafNode *>(a);
    const auto *lb = static_cast<const LeafNode *>(b);
    uint16_t m = la->presentMask;
    while (m) {
      unsigned s = static_cast<unsigned>(__builtin_ctz(m));
      m &= static_cast<uint16_t>(m - 1u);
      if (la->keys[s] != lb->keys[s] || la->vals[s] != lb->vals[s])
        return false;
    }
    return true;
  }
  const auto *ba = static_cast<const BranchNode *>(a);
  const auto *bb = static_cast<const BranchNode *>(b);
  uint16_t m = ba->presentMask;
  while (m) {
    unsigned s = static_cast<unsigned>(__builtin_ctz(m));
    m &= static_cast<uint16_t>(m - 1u);
    if (!nodesEqual(ba->children[s], bb->children[s]))
      return false;
  }
  return true;
}

void Store::const_iterator::descendLeft(const Node *n) {
  while (n) {
    uint16_t mask = n->presentMask;
    if (mask == 0)
      return;
    unsigned s = static_cast<unsigned>(__builtin_ctz(mask));
    uint16_t rest = static_cast<uint16_t>(mask & (mask - 1u));
    stack[depth++] = Frame{n, rest};
    if (n->level == 0) {
      const auto *leaf = static_cast<const LeafNode *>(n);
      current = {leaf->keys[s], leaf->vals[s]};
      return;
    }
    n = static_cast<const BranchNode *>(n)->children[s];
  }
}

Store::const_iterator::const_iterator(const Node *rootNode) {
  if (rootNode)
    descendLeft(rootNode);
}

void Store::const_iterator::advance() {
  while (depth > 0) {
    auto &top = stack[depth - 1];
    if (top.remaining == 0) {
      --depth;
      continue;
    }
    unsigned s = static_cast<unsigned>(__builtin_ctz(top.remaining));
    top.remaining = static_cast<uint16_t>(top.remaining & (top.remaining - 1u));
    if (top.node->level == 0) {
      const auto *leaf = static_cast<const LeafNode *>(top.node);
      current = {leaf->keys[s], leaf->vals[s]};
      return;
    }
    descendLeft(static_cast<const BranchNode *>(top.node)->children[s]);
    return;
  }
  current = {nullptr, PtsSet::getEmptySet()};
}

} // namespace tpa
