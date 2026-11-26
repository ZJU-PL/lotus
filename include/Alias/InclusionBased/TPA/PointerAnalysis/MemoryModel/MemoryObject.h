#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryBlock.h"
#include "Alias/InclusionBased/TPA/Util/Hashing.h"

#include <cstddef>

namespace tpa {

// Memory object representation
//
// A MemoryObject represents a specific memory location that pointers can point
// to. Conceptually, it's a pair (MemoryBlock, offset), representing a field or
// element within a memory allocation.
//
// Field Sensitivity:
// - Each field of a struct is a separate MemoryObject
// - Each element of an array is a separate MemoryObject
// - This enables precise analysis of struct/array access
//
// Summary Objects:
// - For recursive types or when precision is lost, summary objects may be used
// - Summary objects represent "some part of this allocation"
//
// Example:
//   struct Point { int x; int y; };
//   Point p;
//   &p.x is MemoryObject(p_block, offset_of(x))
//   &p.y is MemoryObject(p_block, offset_of(y))
class MemoryObject {
private:
  const MemoryBlock *memBlock;
  size_t offset;
  bool summary;

  // Private constructor - created by MemoryManager only
  MemoryObject(const MemoryBlock *b, size_t o, bool s)
      : memBlock(b), offset(o), summary(s) {
    assert(b != nullptr);
  }

public:
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
