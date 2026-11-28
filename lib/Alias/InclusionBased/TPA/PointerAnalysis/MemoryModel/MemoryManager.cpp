// Implementation of the MemoryManager.
//
// The MemoryManager is the central authority for managing the abstract memory
// model. It handles the creation of MemoryObjects and their organization into
// MemoryBlocks.
//
// Concepts:
// - MemoryBlock: Represents a conceptual allocation unit (e.g., a malloc call
// site,
//   a stack variable declaration, or a global variable).
// - MemoryObject: Represents a specific offset within a MemoryBlock (e.g.,
// field 'x'
//   at offset 0 inside struct 'Point').
// - TypeLayout: Used to understand the structure (fields, arrays) of allocated
// memory.
//
// Key Features:
// - Region-based Allocation: Distinct handling for Stack, Heap, Global, and
// Function memory.
// - Field Sensitivity: `offsetMemory` creates new MemoryObjects for field
// accesses.
// - Array Abstraction: Detects array accesses via `TypeLayout` and collapses
// them
//   into summary objects (field-insensitive for array elements, but sensitive
//   for the array itself).

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"

using namespace context;

namespace tpa {

// Helper to check if the type layout implies that the object at offset 0
// should be treated as a summary (e.g., an array at the start of a struct).
static bool startWithSummary(const TypeLayout *type) {
  bool ret;
  std::tie(std::ignore, ret) = type->offsetInto(0);
  return ret;
}

MemoryManager::MemoryManager(size_t pSize)
    : ptrSize(pSize), argvObj(nullptr), envpObj(nullptr) {}

// Internal factory for MemoryObjects.
// Interns the object in `objSet` to ensure pointer uniqueness.
// Also maintains the blockToObjects index so that
// getReachableMemoryObjects() can look up objects by block in O(k) time
// instead of scanning all objects in O(n).
const MemoryObject *
MemoryManager::getMemoryObjectUnlocked(const MemoryBlock *memBlock,
                                       size_t offset, bool summary) const {
  assert(memBlock != nullptr);

  auto obj = MemoryObject(memBlock, offset, summary);
  auto insertResult = objSet.insert(obj);
  auto itr = insertResult.first;
  bool inserted = insertResult.second;
  assert(itr->isSummaryObject() == summary);
  const MemoryObject *result = &*itr;

  if (inserted) {
    result->id = nextObjId++;
    blockToObjects[memBlock].push_back(result);
  }

  return result;
}

const MemoryObject *MemoryManager::getMemoryObject(const MemoryBlock *memBlock,
                                                   size_t offset,
                                                   bool summary) const {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  return getMemoryObjectUnlocked(memBlock, offset, summary);
}

// Internal factory for MemoryBlocks.
// Maps an AllocSite (instruction + context) to a MemoryBlock.
const MemoryBlock *
MemoryManager::allocateMemoryBlockUnlocked(AllocSite allocSite,
                                           const TypeLayout *type) {
  auto itr = allocMap.find(allocSite);
  if (itr == allocMap.end())
    itr = allocMap.insert(
        itr, std::make_pair(allocSite, MemoryBlock(allocSite, type)));
  assert(type == itr->second.getTypeLayout());
  return &itr->second;
}

const MemoryBlock *MemoryManager::allocateMemoryBlock(AllocSite allocSite,
                                                      const TypeLayout *type) {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  return allocateMemoryBlockUnlocked(allocSite, type);
}

// Allocates memory for a global variable.
// Globals are context-insensitive (Global AllocSite).
const MemoryObject *
MemoryManager::allocateGlobalMemory(const llvm::GlobalVariable *value,
                                    const TypeLayout *type) {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  assert(value != nullptr && type != nullptr);

  const auto *memBlock =
      allocateMemoryBlockUnlocked(AllocSite::getGlobalAllocSite(value), type);
  return getMemoryObjectUnlocked(memBlock, 0, startWithSummary(type));
}

// Allocates memory for a function (to support function pointers).
// Treats the function as a memory block with 0 size.
const MemoryObject *
MemoryManager::allocateMemoryForFunction(const llvm::Function *f) {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  const auto *memBlock =
      allocateMemoryBlockUnlocked(AllocSite::getFunctionAllocSite(f),
                                  TypeLayout::getPointerTypeLayoutWithSize(0));
  return getMemoryObjectUnlocked(memBlock, 0, false);
}

// Allocates stack memory (alloca).
// Context-sensitive based on the current context.
const MemoryObject *MemoryManager::allocateStackMemory(const Context *ctx,
                                                       const llvm::Value *ptr,
                                                       const TypeLayout *type) {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  const auto *memBlock =
      allocateMemoryBlockUnlocked(AllocSite::getStackAllocSite(ctx, ptr), type);
  return getMemoryObjectUnlocked(memBlock, 0, startWithSummary(type));
}

// Allocates heap memory (malloc/new).
// Context-sensitive. Heap objects are ALWAYS summary objects (weak updates)
// because multiple runtime allocations map to the same abstract object.
const MemoryObject *MemoryManager::allocateHeapMemory(const Context *ctx,
                                                      const llvm::Value *ptr,
                                                      const TypeLayout *type) {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  const auto *memBlock =
      allocateMemoryBlockUnlocked(AllocSite::getHeapAllocSite(ctx, ptr), type);
  // Note: summary=true passed to getMemoryObject
  return getMemoryObjectUnlocked(memBlock, 0, true);
}

// Special allocation for argv (array of strings).
const MemoryObject *MemoryManager::allocateArgv(const llvm::Value *ptr) {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  const auto *memBlock = allocateMemoryBlockUnlocked(
      AllocSite::getStackAllocSite(Context::getGlobalContext(), ptr),
      TypeLayout::getByteArrayTypeLayout());
  argvObj = getMemoryObjectUnlocked(memBlock, 0, true);
  return argvObj;
}

// Special allocation for envp (environment pointer).
const MemoryObject *MemoryManager::allocateEnvp(const llvm::Value *ptr) {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  const auto *memBlock = allocateMemoryBlockUnlocked(
      AllocSite::getStackAllocSite(Context::getGlobalContext(), ptr),
      TypeLayout::getByteArrayTypeLayout());
  envpObj = getMemoryObjectUnlocked(memBlock, 0, true);
  return envpObj;
}

// Calculates a new MemoryObject given a base object and an offset.
// This is the core of field sensitivity.
const MemoryObject *MemoryManager::offsetMemory(const MemoryObject *obj,
                                                size_t offset) const {
  assert(obj != nullptr);

  if (offset == 0)
    return obj;
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  return offsetMemoryUnlocked(obj->getMemoryBlock(), obj->getOffset() + offset);
}

// Low-level offset calculation.
// Determines the new offset and whether it falls into an array (requiring
// summary).
const MemoryObject *MemoryManager::offsetMemoryUnlocked(
    const MemoryBlock *block, size_t offset) const {
  assert(block != nullptr);

  // Universal/Null blocks are invariant to offset.
  if (block == &uBlock || block == &nBlock)
    return &uObj;

  const auto *type = block->getTypeLayout();

  bool summary;
  size_t adjustedOffset;
  // Consult TypeLayout to see if 'offset' hits an array region.
  // adjustedOffset will be normalized (modulo element size) if inside an array.
  std::tie(adjustedOffset, summary) = type->offsetInto(offset);

  // Heap objects are inherently summary objects.
  summary = summary || block->isHeapBlock();

  // Bounds check
  if (adjustedOffset >= type->getSize())
    return &uObj;

  return getMemoryObjectUnlocked(block, adjustedOffset, summary);
}

const MemoryObject *MemoryManager::offsetMemory(const MemoryBlock *block,
                                                size_t offset) const {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  return offsetMemoryUnlocked(block, offset);
}

std::vector<const MemoryObject *>
MemoryManager::offsetMemoryArray(const MemoryObject *srcObj,
                                 size_t stride) const {
  assert(srcObj != nullptr);
  std::vector<const MemoryObject *> result;
  if (stride == 0) {
    result.push_back(srcObj);
    return result;
  }

  const auto *block = srcObj->getMemoryBlock();
  if (block == &uBlock || block == &nBlock) {
    result.push_back(&uObj);
    return result;
  }

  const auto *type = block->getTypeLayout();
  const size_t objSize = type->getSize();
  const size_t base = srcObj->getOffset();
  if (base >= objSize)
    return result;

  const auto folded =
      type->getArrayLayout()->collectPeriodicOffsets(base, objSize, stride);
  const bool isHeap = block->isHeapBlock();

  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  result.reserve(folded.size());
  for (const auto &entry : folded) {
    if (entry.first >= objSize)
      result.push_back(&uObj);
    else
      result.push_back(
          getMemoryObjectUnlocked(block, entry.first, entry.second || isHeap));
  }
  return result;
}

llvm::ArrayRef<const MemoryObject *>
MemoryManager::getReachablePointerObjectsRef(const MemoryObject *obj,
                                             bool includeSelf) const {
  assert(obj != nullptr);
  // Never store a MemoryManager-owned cache pointer on static special objects.
  if (obj->isSpecialObject()) {
    if (!includeSelf)
      return {};
    static const MemoryObject *const kUniversalArr[1] = {&uObj};
    static const MemoryObject *const kNullArr[1] = {&nObj};
    return obj == &uObj ? llvm::ArrayRef<const MemoryObject *>(kUniversalArr, 1)
                        : llvm::ArrayRef<const MemoryObject *>(kNullArr, 1);
  }

  const auto *cached = obj->reachablePtrCache.load(std::memory_order_acquire);
  if (cached != nullptr) {
    llvm::ArrayRef<const MemoryObject *> ref(*cached);
    return includeSelf ? ref : ref.drop_front(1);
  }

  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();

  cached = obj->reachablePtrCache.load(std::memory_order_relaxed);
  if (cached == nullptr) {
    std::vector<const MemoryObject *> vec;
    vec.push_back(obj);
    const auto *memBlock = obj->getMemoryBlock();
    const auto *ptrLayout = memBlock->getTypeLayout()->getPointerLayout();
    auto itr = ptrLayout->lower_bound(obj->getOffset());
    if (itr != ptrLayout->end() && *itr == obj->getOffset())
      ++itr;
    vec.reserve(1u + static_cast<size_t>(std::distance(itr, ptrLayout->end())));
    for (; itr != ptrLayout->end(); ++itr)
      vec.push_back(offsetMemoryUnlocked(memBlock, *itr));
    reachablePtrStorage.push_back(std::move(vec));
    cached = &reachablePtrStorage.back();
    obj->reachablePtrCache.store(cached, std::memory_order_release);
  }

  llvm::ArrayRef<const MemoryObject *> ref(*cached);
  return includeSelf ? ref : ref.drop_front(1);
}

// Returns all pointer-typed fields reachable from 'obj'.
// Used for copying (e.g., struct assignment) to ensure deep copies of pointers.
std::vector<const MemoryObject *>
MemoryManager::getReachablePointerObjects(const MemoryObject *obj,
                                          bool includeSelf) const {
  auto ref = getReachablePointerObjectsRef(obj, includeSelf);
  return std::vector<const MemoryObject *>(ref.begin(), ref.end());
}

// Returns all abstract memory objects that belong to the same MemoryBlock
// as 'obj' and are currently instantiated in the manager.
//
// Use the blockToObjects index (populated in getMemoryObject) to
// look up objects by block in O(k) time instead of scanning all objects in
// O(n). This is a significant performance improvement for large programs
// where the total number of memory objects can be in the millions.
std::vector<const MemoryObject *>
MemoryManager::getReachableMemoryObjects(const MemoryObject *obj) const {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  if (obj->isSpecialObject())
    return {obj};

  const auto *block = obj->getMemoryBlock();
  auto itr = blockToObjects.find(block);
  if (itr == blockToObjects.end())
    return {};
  return itr->second;
}

} // namespace tpa