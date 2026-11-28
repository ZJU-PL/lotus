#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryObject.h"
#include "Checker/TPA/Lattice/TaintLattice.h"

#include <cstddef>
#include <cstdint>

#include <llvm/ADT/DenseMap.h>

namespace tpa::taint {

// Compact 2-bit-per-MemoryObject taint store keyed by chunk index (32 objects
// per 64-bit word). Because TaintLattice encodes Unknown=0, Untainted=1,
// Tainted=2, Either=3, lattice join across 32 objects is a single bitwise OR.
class TaintStore {
private:
  llvm::SmallDenseMap<uint32_t, uint64_t, 4> chunkMap;

public:
  static constexpr uint64_t kOddLaneMask = 0x5555555555555555ULL;

  static constexpr uint32_t chunkIndex(uint32_t objId) { return objId >> 5; }
  static constexpr unsigned chunkShift(uint32_t objId) {
    return (objId & 31u) << 1;
  }

  TaintStore() = default;

  TaintLattice lookupById(uint32_t objId) const {
    auto itr = chunkMap.find(chunkIndex(objId));
    if (itr == chunkMap.end())
      return TaintLattice::Unknown;
    return static_cast<TaintLattice>((itr->second >> chunkShift(objId)) & 3ULL);
  }

  TaintLattice lookup(const tpa::MemoryObject *obj) const {
    return lookupById(obj->getId());
  }

  uint64_t lookupChunk(uint32_t chunkIdx) const {
    auto itr = chunkMap.find(chunkIdx);
    return itr == chunkMap.end() ? 0ULL : itr->second;
  }

  bool weakUpdateById(uint32_t objId, TaintLattice l) {
    const uint64_t bits = static_cast<uint64_t>(l);
    if (bits == 0ULL)
      return false;
    const uint32_t cIdx = chunkIndex(objId);
    const uint64_t shifted = bits << chunkShift(objId);
    auto res = chunkMap.try_emplace(cIdx, shifted);
    if (res.second)
      return true;
    const uint64_t prev = res.first->second;
    const uint64_t next = prev | shifted;
    if (next == prev)
      return false;
    res.first->second = next;
    return true;
  }

  bool weakUpdate(const tpa::MemoryObject *obj, TaintLattice l) {
    return weakUpdateById(obj->getId(), l);
  }

  bool strongUpdateById(uint32_t objId, TaintLattice l) {
    const uint32_t cIdx = chunkIndex(objId);
    const unsigned shift = chunkShift(objId);
    const uint64_t bits = static_cast<uint64_t>(l) << shift;
    const uint64_t clearMask = ~(3ULL << shift);
    auto itr = chunkMap.find(cIdx);
    if (itr == chunkMap.end()) {
      if (bits == 0ULL)
        return false;
      chunkMap.try_emplace(cIdx, bits);
      return true;
    }
    const uint64_t prev = itr->second;
    const uint64_t next = (prev & clearMask) | bits;
    if (next == prev)
      return false;
    if (next == 0ULL)
      chunkMap.erase(itr);
    else
      itr->second = next;
    return true;
  }

  bool strongUpdate(const tpa::MemoryObject *obj, TaintLattice l) {
    return strongUpdateById(obj->getId(), l);
  }

  // Merges `valWord` into chunk `chunkIdx` and returns the exact bit diff
  // (non-zero iff at least one 2-bit lane grew).
  uint64_t mergeChunk(uint32_t chunkIdx, uint64_t valWord) {
    if (valWord == 0ULL)
      return 0ULL;
    auto res = chunkMap.try_emplace(chunkIdx, valWord);
    if (res.second)
      return valWord;
    const uint64_t prev = res.first->second;
    const uint64_t next = prev | valWord;
    res.first->second = next;
    return next ^ prev;
  }

  bool mergeWith(const TaintStore &other) {
    bool changed = false;
    for (const auto &entry : other.chunkMap)
      changed |= (mergeChunk(entry.first, entry.second) != 0ULL);
    return changed;
  }

  const auto &chunks() const { return chunkMap; }
  size_t numChunks() const { return chunkMap.size(); }
  void reserveChunks(size_t n) { chunkMap.reserve(n); }
  bool empty() const { return chunkMap.empty(); }

  size_t size() const {
    size_t total = 0;
    for (const auto &entry : chunkMap) {
      const uint64_t w = entry.second;
      total += static_cast<size_t>(
          __builtin_popcountll((w | (w >> 1)) & kOddLaneMask));
    }
    return total;
  }

  template <typename Fn> void forEachNonZero(Fn &&fn) const {
    for (const auto &entry : chunkMap) {
      const uint32_t baseId = entry.first << 5;
      const uint64_t w = entry.second;
      uint64_t lanes = (w | (w >> 1)) & kOddLaneMask;
      while (lanes) {
        const unsigned tz = static_cast<unsigned>(__builtin_ctzll(lanes));
        fn(baseId | (tz >> 1), static_cast<TaintLattice>((w >> tz) & 3ULL));
        lanes &= (lanes - 1);
      }
    }
  }

  template <typename Fn> void forEachEither(Fn &&fn) const {
    for (const auto &entry : chunkMap) {
      const uint32_t baseId = entry.first << 5;
      const uint64_t w = entry.second;
      uint64_t eitherLanes = (w & (w >> 1)) & kOddLaneMask;
      while (eitherLanes) {
        const unsigned tz = static_cast<unsigned>(__builtin_ctzll(eitherLanes));
        fn(baseId | (tz >> 1));
        eitherLanes &= (eitherLanes - 1);
      }
    }
  }
};

} // namespace tpa::taint