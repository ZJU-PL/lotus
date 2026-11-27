#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryObject.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/PtsSet.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseSet.h>

namespace tpa {

// Persistent, structurally-shared radix-16 memory store.
//
// Maps each abstract MemoryObject* (keyed by its dense uint32_t ID) to a
// PtsSet. Subtrees are reference-counted and immutable when shared:
// - Copying a Store is O(1) (single root pointer retain).
// - Point updates path-copy only O(log_16 N) <= 4 small nodes (or mutate
//   in-place when uniquely owned).
// - mergeWith() skips identical shared subtrees in O(1) via pointer equality
//   (nodeA == nodeB), reducing merge cost from O(|Store|) to O(Delta log N).
// - Per-node summary masks (nonAccCount, nonAccChildMask, accToNonAccMask)
//   allow StorePruner to skip pure-global subtrees in O(1).
class Store {
public:
  static constexpr unsigned kBits = 4;
  static constexpr unsigned kFanout = 1u << kBits; // 16
  static constexpr unsigned kMask = kFanout - 1u;  // 0xF
  static constexpr unsigned kMaxLevels = 8;        // 8 * 4 = 32 bits

  struct Node {
    mutable std::atomic<uint32_t> refCount{1};
    uint8_t level; // 0 = LeafNode, >= 1 = BranchNode
    uint16_t presentMask = 0;
    uint16_t nonAccChildMask = 0;
    uint16_t accToNonAccMask = 0;
    uint32_t totalCount = 0;
    uint32_t nonAccCount = 0;
    uint64_t versionId;

    explicit Node(uint8_t lvl) : level(lvl), versionId(allocVersionId()) {}
  };

  static uint64_t allocVersionId() noexcept {
    static std::atomic<uint64_t> nextVersion{1};
    return nextVersion.fetch_add(1, std::memory_order_relaxed);
  }

  struct LeafNode : Node {
    const MemoryObject *keys[kFanout]{};
    PtsSet vals[kFanout]{};

    LeafNode() : Node(0) {}
    LeafNode(const LeafNode &other);
  };

  struct BranchNode : Node {
    const Node *children[kFanout]{};

    explicit BranchNode(uint8_t lvl) : Node(lvl) {}
    BranchNode(const BranchNode &other);
  };

private:
  const Node *root = nullptr;

  static const Node *retain(const Node *n) noexcept {
    if (n)
      n->refCount.fetch_add(1, std::memory_order_relaxed);
    return n;
  }
  static void release(const Node *n) noexcept;
  static void destroyNode(const Node *n) noexcept;

  static bool isAccessibleObj(const MemoryObject *obj) noexcept {
    return !(obj->isStackObject() || obj->isHeapObject());
  }

  static uint64_t capacityForLevel(uint8_t level) noexcept {
    const unsigned shift = static_cast<unsigned>(level + 1u) * kBits;
    return shift >= 32u ? (1ULL << 32) : (1ULL << shift);
  }

  void ensureCapacity(uint32_t id);

  static const Node *updateNode(const Node *node, uint8_t level, uint32_t id,
                                const MemoryObject *key, PtsSet value,
                                bool isStrong, bool canMutate, bool &changed);
  static const Node *mergeSameLevel(const Node *a, const Node *b,
                                    bool canMutateA, bool &changed);
  static const Node *mergeLowerIntoHigher(const Node *a, const Node *b,
                                          bool canMutateA, bool &changed);
  static const Node *filterNodeRange(const Node *node, uint64_t baseId,
                                     const uint32_t *lo, const uint32_t *hi);
  static bool nodesEqual(const Node *a, const Node *b);

public:
  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<const MemoryObject *, PtsSet>;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type *;
    using reference = const value_type &;

  private:
    struct Frame {
      const Node *node;
      uint16_t remaining;
    };
    Frame stack[kMaxLevels]{};
    unsigned depth = 0;
    value_type current{nullptr, PtsSet::getEmptySet()};

    void descendLeft(const Node *n);
    void advance();

  public:
    const_iterator() = default;
    explicit const_iterator(const Node *rootNode);

    reference operator*() const { return current; }
    pointer operator->() const { return &current; }

    const_iterator &operator++() {
      advance();
      return *this;
    }
    const_iterator operator++(int) {
      const_iterator tmp = *this;
      advance();
      return tmp;
    }
    bool operator==(const const_iterator &rhs) const {
      if (depth != rhs.depth)
        return false;
      if (depth == 0)
        return true;
      return stack[depth - 1].node == rhs.stack[depth - 1].node &&
             current.first == rhs.current.first;
    }
    bool operator!=(const const_iterator &rhs) const { return !(*this == rhs); }
  };

  Store() = default;
  Store(const Store &other) noexcept : root(retain(other.root)) {}
  Store(Store &&other) noexcept : root(other.root) { other.root = nullptr; }
  Store &operator=(const Store &other) noexcept {
    if (this != &other) {
      const Node *old = root;
      root = retain(other.root);
      release(old);
    }
    return *this;
  }
  Store &operator=(Store &&other) noexcept {
    if (this != &other) {
      release(root);
      root = other.root;
      other.root = nullptr;
    }
    return *this;
  }
  ~Store() { release(root); }

  bool isOverlay() const { return false; }
  void reserve(size_t) {}
  bool sameRoot(const Store &other) const { return root == other.root; }

  const PtsSet *findOwnBinding(const MemoryObject *key) const;
  PtsSet lookup(const MemoryObject *key) const {
    const PtsSet *found = findOwnBinding(key);
    return found ? *found : PtsSet::getEmptySet();
  }
  bool contains(const MemoryObject *key) const { return !lookup(key).empty(); }
  bool hasBinding(const MemoryObject *key) const {
    return findOwnBinding(key) != nullptr;
  }

  bool insert(const MemoryObject *key, const MemoryObject *obj);
  bool weakUpdate(const MemoryObject *key, PtsSet pSet);
  bool strongUpdate(const MemoryObject *key, PtsSet pSet);
  bool mergeWith(const Store &rhs);

  bool sameBindings(const Store &other) const {
    return nodesEqual(root, other.root);
  }

  size_t size() const { return root ? root->totalCount : 0; }
  bool empty() const { return root == nullptr; }
  size_t nonAccessibleCount() const { return root ? root->nonAccCount : 0; }

  // Invokes `callback(ptsSet)` for every accessible (global/function) binding
  // whose PtsSet contains at least one stack/heap object, skipping all other
  // subtrees in O(1).
  template <typename Callback>
  void forEachAccessiblePointingToNonAccessible(Callback &&callback) const {
    if (!root || root->accToNonAccMask == 0)
      return;
    visitAccToNonAccImpl(root, callback);
  }

  // Returns a new Store keeping all accessible bindings plus non-accessible
  // bindings whose IDs are present in sorted `sortedIds`, structurally sharing
  // pure-global subtrees and dropping pure-unreachable subtrees in O(1).
  Store filterNonAccessibleIds(llvm::ArrayRef<uint32_t> sortedIds) const;
  Store filterNonAccessible(
      const llvm::DenseSet<const MemoryObject *> &reachableNonAcc) const;

  const_iterator begin() const { return const_iterator(root); }
  const_iterator end() const { return const_iterator(); }

private:
  template <typename Callback>
  static void visitAccToNonAccImpl(const Node *node, Callback &callback) {
    uint16_t mask = node->accToNonAccMask;
    if (node->level == 0) {
      const auto *leaf = static_cast<const LeafNode *>(node);
      while (mask) {
        unsigned s = static_cast<unsigned>(__builtin_ctz(mask));
        mask &= static_cast<uint16_t>(mask - 1u);
        callback(leaf->vals[s]);
      }
      return;
    }
    const auto *branch = static_cast<const BranchNode *>(node);
    while (mask) {
      unsigned s = static_cast<unsigned>(__builtin_ctz(mask));
      mask &= static_cast<uint16_t>(mask - 1u);
      visitAccToNonAccImpl(branch->children[s], callback);
    }
  }
};

} // namespace tpa
