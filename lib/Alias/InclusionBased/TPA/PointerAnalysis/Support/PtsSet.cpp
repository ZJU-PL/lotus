#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/PtsSet.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace tpa {

namespace {
struct PtsMergeCacheEntry {
  const void *a = nullptr;
  const void *b = nullptr;
  const void *res = nullptr;
};
constexpr size_t kPtsMergeCacheSize = 8192;
thread_local std::array<PtsMergeCacheEntry, kPtsMergeCacheSize> ptsMergeCache{};
} // namespace

std::array<PtsSet::Shard, 32> PtsSet::existingSets;
const PtsSet::InternedSet *PtsSet::emptySet = uniquifySet(SetType{});

const PtsSet::InternedSet *PtsSet::uniquifySet(SetType &&set) {
  if (set.empty() && emptySet != nullptr)
    return emptySet;
  if (set.count(MemoryManager::getUniversalObject()))
    set = {MemoryManager::getUniversalObject()};

  bool nonAcc = false;
  for (const auto *obj : set) {
    if (obj->isStackObject() || obj->isHeapObject()) {
      nonAcc = true;
      break;
    }
  }
  InternedSet candidate(std::move(set), nonAcc);
  auto &shard = existingSets[InternedSetHasher{}(candidate) %
                             existingSets.size()];
  std::lock_guard<std::mutex> lock(shard.mutex);
  auto &existingSet = shard.sets;

  auto itr = existingSet.find(candidate);
  if (itr == existingSet.end()) {
    candidate.objs.shrink_to_fit();
    itr = existingSet.insert(itr, std::move(candidate));
  }

  return &*itr;
}

PtsSet PtsSet::insert(const MemoryObject *obj) const {
  if (pSet->objs.count(obj))
    return *this;
  const auto *universal = MemoryManager::getUniversalObject();
  if (pSet->objs.size() == 1 && pSet->objs.front() == universal)
    return *this;
  if (obj == universal)
    return getSingletonSet(universal);

  SetType newSet(pSet->objs);
  newSet.insert(obj);

  return PtsSet(uniquifySet(std::move(newSet)));
}

PtsSet PtsSet::merge(const PtsSet &rhs) const {
  // The easy case
  if (pSet == rhs.pSet)
    return *this;
  if (pSet == emptySet)
    return rhs;
  if (rhs.pSet == emptySet)
    return *this;
  // Sets containing Universal normalize to this singleton. Preserve the
  // lattice join without allocating a temporary vector or taking an intern
  // lock.
  const auto *universal = MemoryManager::getUniversalObject();
  if (pSet->objs.size() == 1 && pSet->objs.front() == universal)
    return *this;
  if (rhs.pSet->objs.size() == 1 && rhs.pSet->objs.front() == universal)
    return rhs;

  // Thread-local direct-mapped commutative cache of interned (a, b) -> union.
  // InternedSet addresses live for the process lifetime, so hits are exact.
  const InternedSet *p1 = pSet;
  const InternedSet *p2 = rhs.pSet;
  if (p1 > p2)
    std::swap(p1, p2);
  const size_t cacheSlot =
      (((reinterpret_cast<uintptr_t>(p1) >> 4) * 11400714819323198485ULL) ^
       ((reinterpret_cast<uintptr_t>(p2) >> 4) * 14029467366897019727ULL)) &
      (kPtsMergeCacheSize - 1u);
  auto &cached = ptsMergeCache[cacheSlot];
  if (cached.a == p1 && cached.b == p2)
    return PtsSet(static_cast<const InternedSet *>(cached.res));

  // Subset fast paths avoid heap allocation and interning locks when one set
  // already subsumes the other (very common at CFG merge points). Equal-sized
  // distinct interned sets cannot subsume each other.
  const size_t lhsSize = pSet->objs.size();
  const size_t rhsSize = rhs.pSet->objs.size();
  if (lhsSize > rhsSize) {
    if (rhsSize == 1u) {
      if (pSet->objs.count(rhs.pSet->objs.front())) {
        cached = {p1, p2, pSet};
        return *this;
      }
    } else if (pSet->objs.front() <= rhs.pSet->objs.front() &&
               pSet->objs.back() >= rhs.pSet->objs.back() &&
               pSet->objs.includes(rhs.pSet->objs)) {
      cached = {p1, p2, pSet};
      return *this;
    }
  } else if (rhsSize > lhsSize) {
    if (lhsSize == 1u) {
      if (rhs.pSet->objs.count(pSet->objs.front())) {
        cached = {p1, p2, rhs.pSet};
        return rhs;
      }
    } else if (rhs.pSet->objs.front() <= pSet->objs.front() &&
               rhs.pSet->objs.back() >= pSet->objs.back() &&
               rhs.pSet->objs.includes(pSet->objs)) {
      cached = {p1, p2, rhs.pSet};
      return rhs;
    }
  }

  std::vector<const MemoryObject *> merged;
  merged.reserve(lhsSize + rhsSize);
  std::set_union(pSet->objs.begin(), pSet->objs.end(), rhs.pSet->objs.begin(),
                 rhs.pSet->objs.end(), std::back_inserter(merged));
  const InternedSet *res =
      uniquifySet(SetType::fromSortedUnique(std::move(merged)));
  cached = {p1, p2, res};
  return PtsSet(res);
}

PtsSet PtsSet::getEmptySet() { return PtsSet(emptySet); }

PtsSet PtsSet::getSingletonSet(const MemoryObject *obj) {
  SetType newSet = {obj};
  return PtsSet(uniquifySet(std::move(newSet)));
}

PtsSet PtsSet::fromObjects(std::vector<const MemoryObject *> objects) {
  if (objects.empty())
    return getEmptySet();
  const auto *universal = MemoryManager::getUniversalObject();
  for (const auto *obj : objects)
    if (obj == universal)
      return getSingletonSet(universal);
  return PtsSet(uniquifySet(SetType(std::move(objects))));
}

std::vector<const MemoryObject *> PtsSet::intersects(const PtsSet &s0,
                                                     const PtsSet &s1) {
  return SetType::intersects(s0.pSet->objs, s1.pSet->objs);
}

PtsSet PtsSet::mergeAll(const std::vector<PtsSet> &sets) {
  if (sets.empty())
    return getEmptySet();
  if (sets.size() == 1)
    return sets[0];
  if (sets.size() == 2)
    return sets[0].merge(sets[1]);

  const auto *universal = MemoryManager::getUniversalObject();
  size_t totSize = 0;
  size_t nonEmptyCount = 0;
  PtsSet singleNonEmpty = getEmptySet();
  for (auto const &pSet : sets) {
    if (pSet.empty())
      continue;
    if (pSet.size() == 1 && *pSet.begin() == universal)
      return pSet;
    if (nonEmptyCount == 0) {
      singleNonEmpty = pSet;
      nonEmptyCount = 1;
    } else if (singleNonEmpty != pSet) {
      ++nonEmptyCount;
    }
    totSize += pSet.size();
  }
  if (nonEmptyCount == 0)
    return getEmptySet();
  if (nonEmptyCount == 1)
    return singleNonEmpty;

  std::vector<const MemoryObject *> flat;
  flat.reserve(totSize);
  for (auto const &pSet : sets)
    flat.insert(flat.end(), pSet.begin(), pSet.end());

  return PtsSet(uniquifySet(SetType(std::move(flat))));
}

} // namespace tpa
