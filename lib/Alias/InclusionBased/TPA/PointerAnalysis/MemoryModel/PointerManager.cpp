// Implementation of the PointerManager.
//
// The PointerManager is responsible for managing the `Pointer` abstraction.
// A `Pointer` in TPA represents an SSA value (register) in a specific context.
//
// Key Responsibilities:
// 1. Value Canonicalization: Handling LLVM value quirks (stripping bitcasts,
//    handling trivial PHIs) to map equivalent values to the same Pointer.
// 2. Pointer Interning: Maintaining a unique set of Pointer objects to allow
//    fast pointer equality comparison.
// 3. Mapping: Maintaining the relationship between LLVM Values and TPA
// Pointers.
// 4. Special Pointers: Managing the Universal and Null pointers.

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/PointerManager.h"

#include "Alias/InclusionBased/TPA/Context/Context.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>

using namespace context;

namespace tpa {

// Resolve an extract to the pointer inserted into its aggregate/vector;
// Undef if unresolved.
const llvm::Value *resolveExtractedPointer(const llvm::Value *value) {
  if (const auto *ev = llvm::dyn_cast<llvm::ExtractValueInst>(value)) {
    const llvm::Value *agg = ev->getAggregateOperand();
    while (agg != nullptr) {
      if (const auto *iv = llvm::dyn_cast<llvm::InsertValueInst>(agg)) {
        if (iv->getIndices() == ev->getIndices()) {
          auto *ins = iv->getInsertedValueOperand()->stripPointerCasts();
          if (ins->getType()->isPointerTy())
            return ins;
          break;
        }
        agg = iv->getAggregateOperand();
        continue;
      }
      break;
    }
    return llvm::UndefValue::get(value->getType());
  }

  if (const auto *ee = llvm::dyn_cast<llvm::ExtractElementInst>(value)) {
    const auto *idxC = llvm::dyn_cast<llvm::ConstantInt>(ee->getIndexOperand());
    if (idxC != nullptr) {
      const uint64_t targetIdx = idxC->getZExtValue();
      const llvm::Value *vec = ee->getVectorOperand();
      while (vec != nullptr) {
        if (const auto *ie = llvm::dyn_cast<llvm::InsertElementInst>(vec)) {
          const auto *ieIdxC =
              llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2));
          if (ieIdxC != nullptr && ieIdxC->getZExtValue() == targetIdx) {
            auto *ins = ie->getOperand(1)->stripPointerCasts();
            if (ins->getType()->isPointerTy())
              return ins;
            break;
          }
          vec = ie->getOperand(0);
          continue;
        }
        break;
      }
    }
    return llvm::UndefValue::get(value->getType());
  }

  return value;
}

// Helper to strip non-functional wrappers from an LLVM Value.
// - Strips pointer casts (BitCast, GEP with 0 offset, etc.)
// - Collapses trivial PHI nodes (single operand).
// - Normalizes IntToPtr to Undef (conservative).
const llvm::Value *canonicalizeValue(const llvm::Value *value) {
  assert(value != nullptr);
  value = value->stripPointerCasts();

  if (const auto *phiNode = llvm::dyn_cast<llvm::PHINode>(value)) {
    const llvm::Value *rhs = nullptr;
    for (auto &op : phiNode->operands()) {
      auto *val = op.get()->stripPointerCasts();
      if (rhs == nullptr)
        rhs = val;
      else if (val != rhs) {
        // PHI has different incoming values, cannot simplify.
        rhs = nullptr;
        break;
      }
    }

    if (rhs != nullptr)
      value = rhs;
  } else if (llvm::isa<llvm::IntToPtrInst>(value))
    value = llvm::UndefValue::get(value->getType());

  return value;
}

PointerManager::PointerManager() : uPtr(nullptr), nPtr(nullptr) {}
PointerManager::PointerManager(PointerManager &owner)
    : uPtr(owner.uPtr), nPtr(owner.nPtr), registryMutex(owner.registryMutex),
      base(&owner) {}

bool PointerManager::validateView() const {
  assert(base);
  for (const auto &read : reads)
    if (base->getPointer(read.first.first, read.first.second) != read.second)
      return false;
  return true;
}

void PointerManager::publishView() {
  assert(base);
  std::lock_guard<std::recursive_mutex> lock(*registryMutex);
  base->published.insert(staged.begin(), staged.end());
}

// Creates or retrieves a Pointer object.
// Interns the pointer in `ptrSet`.
const Pointer *PointerManager::buildPointer(const context::Context *ctx,
                                            const llvm::Value *val) {
  if (base) {
    const Pointer *ptr = base->internPointer(ctx, val, false);
    staged.insert(ptr);
    return ptr;
  }
  return internPointer(ctx, val, true);
}

const Pointer *PointerManager::internPointer(const context::Context *ctx,
                                             const llvm::Value *val,
                                             bool visible) {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe)
    lock.lock();
  auto ptr = Pointer(ctx, val);
  auto itr = ptrSet.find(ptr);
  if (itr != ptrSet.end()) {
    if (visible && trackPublished)
      published.insert(&*itr);
    return &*itr;
  }

  itr = ptrSet.insert(itr, ptr);
  const auto *ret = &*itr;
  // Record reverse mapping: Value -> List of Pointers (one per context)
  valuePtrMap[val].push_back(ret);
  if (visible && trackPublished)
    published.insert(ret);
  return ret;
}

// Initializes the universal pointer (represents unknown/all pointers).
const Pointer *PointerManager::setUniversalPointer(const llvm::UndefValue *v) {
  assert(uPtr == nullptr);
  assert(v->getType() == llvm::Type::getInt8PtrTy(v->getContext()));
  uPtr = buildPointer(Context::getGlobalContext(), v);
  return uPtr;
}

const Pointer *PointerManager::getUniversalPointer() const {
  assert(uPtr != nullptr);
  return uPtr;
}

// Initializes the null pointer.
const Pointer *
PointerManager::setNullPointer(const llvm::ConstantPointerNull *v) {
  assert(nPtr == nullptr);
  assert(v->getType() == llvm::Type::getInt8PtrTy(v->getContext()));
  nPtr = buildPointer(Context::getGlobalContext(), v);
  return nPtr;
}

const Pointer *PointerManager::getNullPointer() const {
  assert(nPtr != nullptr);
  return nPtr;
}

// Retrieves an existing pointer. Returns nullptr if not found.
// Handles special cases (Null, Undef/Universal, Globals).
const Pointer *PointerManager::getPointer(const Context *ctx,
                                          const llvm::Value *val) const {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe || base != nullptr)
    lock.lock();
  assert(ctx != nullptr && val != nullptr);

  val = canonicalizeValue(val);

  if (llvm::isa<llvm::ConstantPointerNull>(val))
    return nPtr;
  else if (llvm::isa<llvm::UndefValue>(val))
    return uPtr;
  else if (llvm::isa<llvm::GlobalValue>(val))
    // Globals always live in the global context
    ctx = Context::getGlobalContext();

  const PointerManager &owner = base ? *base : *this;
  const auto itr = owner.ptrSet.find(Pointer(ctx, val));
  const Pointer *canonical = itr == owner.ptrSet.end() ? nullptr : &*itr;
  if (!base && !owner.trackPublished)
    return canonical;
  if (base && canonical && staged.count(canonical))
    return canonical;
  const Pointer *result =
      canonical && owner.published.count(canonical) ? canonical : nullptr;
  if (base)
    reads.emplace(ReadKey{ctx, val}, result);
  return result;
}

// Retrieves a pointer, creating it if it doesn't exist.
const Pointer *PointerManager::getOrCreatePointer(const Context *ctx,
                                                  const llvm::Value *val) {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe || base != nullptr)
    lock.lock();
  assert(ctx != nullptr && val != nullptr);

  val = canonicalizeValue(val);

  if (llvm::isa<llvm::ConstantPointerNull>(val))
    return nPtr;
  else if (llvm::isa<llvm::UndefValue>(val))
    return uPtr;
  else if (llvm::isa<llvm::GlobalValue>(val))
    ctx = Context::getGlobalContext();

  return buildPointer(ctx, val);
}

// Finds all Pointers associated with a given LLVM Value across all contexts.
PointerManager::PointerVector
PointerManager::getPointersWithValue(const llvm::Value *val) const {
  std::unique_lock<std::recursive_mutex> lock(*registryMutex, std::defer_lock);
  if (threadSafe || base != nullptr)
    lock.lock();
  PointerVector vec;

  val = canonicalizeValue(val);

  // The CFG simplifier drops the Copy that models extractvalue/extractelement,
  // so the result is never a Pointer; resolve it to the inserted pointer.
  while (llvm::isa<llvm::ExtractValueInst>(val) ||
         llvm::isa<llvm::ExtractElementInst>(val)) {
    const llvm::Value *resolved = resolveExtractedPointer(val);
    resolved = canonicalizeValue(resolved);
    if (resolved == val)
      break;
    val = resolved;
  }

  if (llvm::isa<llvm::ConstantPointerNull>(val))
    vec.push_back(nPtr);
  else if (llvm::isa<llvm::UndefValue>(val))
    vec.push_back(uPtr);
  else {
    const PointerManager &owner = base ? *base : *this;
    auto itr = owner.valuePtrMap.find(val);
    if (itr != owner.valuePtrMap.end()) {
      if (!base && !owner.trackPublished)
        return itr->second;
      for (const Pointer *pointer : itr->second)
        if (owner.published.count(pointer) || staged.count(pointer))
          vec.push_back(pointer);
    }
  }

  return vec;
}

} // namespace tpa
