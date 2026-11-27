#pragma once

#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"
#include "Alias/InclusionBased/TPA/Util/Hashing.h"

#include <array>
#include <cstddef>
#include <mutex>
#include <unordered_set>

namespace tpa {

class MemoryObject;

// Points-to set representation
//
// A PtsSet represents the set of memory objects that a pointer may point to.
// This is the fundamental data structure for pointer analysis.
//
// Design:
// - Uses flyweight pattern (set interning) for memory efficiency
// - All equal sets share the same underlying SetType pointer
// - Allows fast comparison via pointer equality
// - Immutable: operations return new sets rather than modifying in place
//
// Special Objects:
// - Universal object: represents "may point to anything"
// - Null object: represents the null pointer
// These are managed by MemoryManager
//
// Operations:
// - insert(obj): Add a memory object to the set
// - merge(set): Union with another set
// - has(obj): Membership test
// - includes(set): Subset test
class PtsSet {
private:
  // Underlying set type - VectorSet provides sorted unique storage
  using SetType = util::VectorSet<const MemoryObject *>;

  // Interned entry carrying cached summary metadata computed once at intern
  // time (e.g., whether any element is stack/heap memory for StorePruner).
  struct InternedSet {
    SetType objs;
    bool hasNonAccessible = false;

    InternedSet() = default;
    explicit InternedSet(SetType &&s, bool nonAcc)
        : objs(std::move(s)), hasNonAccessible(nonAcc) {}
    bool operator==(const InternedSet &rhs) const {
      return hasNonAccessible == rhs.hasNonAccessible && objs == rhs.objs;
    }
  };
  struct InternedSetHasher {
    size_t operator()(const InternedSet &entry) const {
      size_t h = util::ContainerHasher<SetType>{}(entry.objs);
      return entry.hasNonAccessible ? (h ^ 0x9e3779b97f4a7c15ULL) : h;
    }
  };

  // Pointer to the interned set data
  const InternedSet *pSet;

  // Flyweight pattern: all equal sets are deduplicated
  using PtsSetSet = std::unordered_set<InternedSet, InternedSetHasher>;
  struct Shard {
    std::mutex mutex;
    PtsSetSet sets;
  };
  static std::array<Shard, 32> existingSets;
  static const InternedSet *emptySet;

  // Private constructor - use factory methods
  PtsSet(const InternedSet *p) : pSet(p) {}

  // Get or create an interned set
  static const InternedSet *uniquifySet(SetType &&set);

public:
  using const_iterator = SetType::const_iterator;

  PtsSet() : pSet(emptySet) {}

  // Add a memory object to this set
  // Returns a new set (immutable design)
  PtsSet insert(const MemoryObject *) const;
  // Union with another set
  // Returns a new set representing the union
  PtsSet merge(const PtsSet &) const;

  // Check if a memory object is in the set
  bool has(const MemoryObject *obj) const { return pSet->objs.count(obj); }
  // Check if this set includes another (subset test)
  bool includes(const PtsSet &rhs) const {
    return pSet->objs.includes(rhs.pSet->objs);
  }
  // True iff the set contains at least one stack or heap MemoryObject.
  bool hasNonAccessible() const { return pSet->hasNonAccessible; }

  // Check if the set is empty
  bool empty() const { return pSet->objs.empty(); }
  // Get the number of elements
  size_t size() const { return pSet->objs.size(); }

  // Equality via pointer comparison (flyweight pattern)
  bool operator==(const PtsSet &rhs) const { return pSet == rhs.pSet; }
  bool operator!=(const PtsSet &rhs) const { return !(*this == rhs); }
  // Iterator over memory objects in the set
  const_iterator begin() const { return pSet->objs.begin(); }
  const_iterator end() const { return pSet->objs.end(); }

  // Factory methods
  static PtsSet getEmptySet();
  static PtsSet getSingletonSet(const MemoryObject *);
  static PtsSet fromObjects(std::vector<const MemoryObject *> objects);
  // Find common elements between two sets
  static std::vector<const MemoryObject *> intersects(const PtsSet &s0,
                                                      const PtsSet &s1);
  // Merge multiple sets into one
  static PtsSet mergeAll(const std::vector<PtsSet> &);

  friend std::hash<PtsSet>;
};

} // namespace tpa

namespace std {
template <> struct hash<tpa::PtsSet> {
  // Hash via pointer to interned set
  size_t operator()(const tpa::PtsSet &p) const {
    return std::hash<const typename tpa::PtsSet::InternedSet *>()(p.pSet);
  }
};
} // namespace std
