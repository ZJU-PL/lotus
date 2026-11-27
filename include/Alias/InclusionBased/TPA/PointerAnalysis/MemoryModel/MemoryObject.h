#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryBlock.h"
#include "Alias/InclusionBased/TPA/Util/Hashing.h"

#include <cstddef>
#include <cstdint>

namespace tpa {

class MemoryObject {
private:
  const MemoryBlock *memBlock;
  size_t offset;
  bool summary;
  mutable uint32_t id;

  // Private constructor - created by MemoryManager only
  MemoryObject(const MemoryBlock *b, size_t o, bool s, uint32_t objId = 0)
      : memBlock(b), offset(o), summary(s), id(objId) {
    assert(b != nullptr);
  }

public:
  // Dense unique ID within a MemoryManager (0 = universal, 1 = null, 2+ =
  // interned objects). Used as radix key for persistent Store trees.
  uint32_t getId() const { return id; }
  // Get the memory block this object belongs to
  const MemoryBlock *getMemoryBlock() const { return memBlock; }
  // Get the offset within the memory block
  size_t getOffset() const { return offset; }
  // Check if this is a summary object (approximation)
  bool isSummaryObject() const { return summary; }

  // Get the allocation site that created this memory
  const AllocSite &getAllocSite() const { return memBlock->getAllocSite(); }

  // Check special objects
  bool isNullObject() const { return memBlock->isNullBlock(); }
  bool isUniversalObject() const { return memBlock->isUniversalBlock(); }
  bool isSpecialObject() const { return isNullObject() || isUniversalObject(); }

  // Check allocation type
  bool isGlobalObject() const { return memBlock->isGlobalBlock(); }
  bool isFunctionObject() const { return memBlock->isFunctionBlock(); }
  bool isStackObject() const { return memBlock->isStackBlock(); }
  bool isHeapObject() const { return memBlock->isHeapBlock(); }

  // Equality: same block and offset
  bool operator==(const MemoryObject &other) const {
    return (memBlock == other.memBlock) && (offset == other.offset);
  }
  bool operator!=(const MemoryObject &other) const { return !(*this == other); }
  // Ordering for use in ordered containers
  bool operator<(const MemoryObject &other) const {
    if (memBlock != other.memBlock)
      return memBlock < other.memBlock;
    else
      return offset < other.offset;
  }

  friend class MemoryManager;
};

} // namespace tpa

namespace std {
template <> struct hash<tpa::MemoryObject> {
  size_t operator()(const tpa::MemoryObject &obj) const {
    return util::hashPair(obj.getMemoryBlock(), obj.getOffset());
  }
};
} // namespace std
