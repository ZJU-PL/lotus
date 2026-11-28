// Implementation of External Call Analysis.
//
// This file handles calls to external functions (functions without definitions
// in the module). Instead of analyzing the body of the function, it relies on
// an "External Pointer Table" (annotations) to model the side effects of the
// call on pointers and memory.
//
// Supported Effects:
// - Alloc: The function allocates memory (like malloc).
// - Copy: The function copies data between pointers (like memcpy, strcpy) or
// assigns values.
// - Exit: The function terminates the program (like exit).
//
// If no annotation is found, the analysis conservatively assumes the function
// acts as a no-op regarding the tracked state (unless it returns a value, which
// is effectively lost/universal). NOTE: A robust analysis should probably
// assume "unknown behavior" escapes everything, but this implementation seems
// to rely on explicit annotations for correctness.

#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/SemiSparseProgram.h"
#include "Annotation/Pointer/ExternalPointerTable.h"
#include "Annotation/Pointer/PointerEffect.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/raw_ostream.h>

using namespace annotation;
using namespace llvm;

namespace tpa {

// Helper to extract the LLVM Value corresponding to a position argument (e.g.,
// Arg(0), Ret).
static const Value *getArgument(const CallCFGNode &callNode,
                                const APosition &pos) {
  const auto *inst = callNode.getCallSite();
  if (pos.isReturnPosition())
    return inst;

  // We can't just call callNode.getArgument(...) because there might be
  // non-pointer args that are not included in callNode
  const auto *callBase = dyn_cast<CallBase>(inst);
  assert(callBase);

  auto argIdx = pos.getAsArgPosition().getArgIndex();
  if (argIdx >= callBase->arg_size())
    return nullptr;

  return callBase->getArgOperand(argIdx)->stripPointerCasts();
}

// Heuristic to determine the type of memory allocated by a malloc-like call.
// Looks at the uses of the call instruction (specifically BitCasts) to infer
// the intended type.
//
// Bug fix: the previous implementation only examined *direct* users of
// the call instruction. A common pattern after optimization passes is:
//
//   %p = call i8* @malloc(...)
//   store i8* %p, i8** %slot          ; direct store — no bitcast visible
//   %q = load SomeType** %slot        ; cast happens at the load site
//   %r = bitcast i8* %p to SomeType*  ; or: bitcast is in a different BB
//
// In such cases the direct-user scan found no BitCastInst and fell back to
// byte-array layout, losing type precision.
//
// Improvement: after scanning direct users, if no unique BitCast type was
// found, also scan users of any StoreInst that stores the malloc result, and
// look at the corresponding load sites for BitCasts. This covers the most
// common indirect pattern without a full def-use traversal.
static Type *getMallocType(const Instruction *callInst) {
  assert(callInst != nullptr);

  // Helper: given a pointer value, collect all BitCast destination types seen
  // among its direct users.
  auto collectBitCastTypes = [](const Value *val, size_t &numBitCasts,
                                size_t &numOther, PointerType *&lastBCType) {
    for (const auto *user : val->users()) {
      if (const auto *bcInst = dyn_cast<BitCastInst>(user)) {
        lastBCType = cast<PointerType>(bcInst->getDestTy());
        numBitCasts++;
      } else if (isa<GetElementPtrInst>(user)) {
        numOther++;
      }
    }
  };

  PointerType *mallocType = nullptr;
  size_t numOfBitCastUses = 0;
  size_t numOfOtherUses = 0;

  // Phase 1: scan direct users of the call instruction.
  collectBitCastTypes(callInst, numOfBitCastUses, numOfOtherUses, mallocType);

  // Phase 2: if no unique BitCast found yet, look through store→load chains.
  // This handles the pattern where the malloc result is stored to a local slot
  // and then loaded (possibly with a different type) elsewhere.
  if (numOfBitCastUses == 0) {
    for (const auto *user : callInst->users()) {
      if (const auto *stInst = dyn_cast<StoreInst>(user)) {
        // The malloc result is stored into a memory slot. Look at all loads
        // from that slot and check their users for BitCasts.
        const auto *slot = stInst->getPointerOperand();
        for (const auto *slotUser : slot->users()) {
          if (const auto *ldInst = dyn_cast<LoadInst>(slotUser)) {
            collectBitCastTypes(ldInst, numOfBitCastUses, numOfOtherUses,
                                mallocType);
          }
        }
      }
    }
  }

  // Exactly one BitCast use and no other ambiguous uses: use the cast's
  // destination element type as the allocation type.
  if (numOfBitCastUses == 1 && mallocType != nullptr)
    return mallocType->getNonOpaquePointerElementType();

  // No BitCast and no GEP uses: the malloc result is used directly as its
  // declared return type (e.g., already the right pointer type).
  if (numOfBitCastUses == 0 && numOfOtherUses == 0)
    return callInst->getType()->getNonOpaquePointerElementType();

  // Multiple or ambiguous uses: type cannot be determined reliably.
  // Return nullptr to fall back to byte-array layout.
  return nullptr;
}

// Checks if the allocation size matches the size of a single instance of the
// type. If not, it's likely an array allocation.
static bool isSingleAlloc(const TypeLayout *typeLayout,
                          const llvm::Value *sizeVal) {
  if (sizeVal == nullptr)
    return false;

  if (const auto *cInt = dyn_cast<ConstantInt>(sizeVal)) {
    auto size = cInt->getZExtValue();
    auto typeSize = typeLayout->getSize();
    if (typeSize == 0 || size % typeSize != 0)
      return false;
    return size == typeSize;
  }

  return false;
}

// Logic for handling malloc-like allocations with a size argument.
bool TransferFunction::evalMallocWithSize(const context::Context *ctx,
                                          const llvm::Instruction *dstVal,
                                          llvm::Type *mallocType,
                                          const llvm::Value *mallocSize) {
  assert(ctx != nullptr && dstVal != nullptr);

  const TypeLayout *typeLayout = nullptr;
  if (mallocType == nullptr)
    typeLayout = TypeLayout::getByteArrayTypeLayout();
  else {
    typeLayout =
        globalState.getSemiSparseProgram().getTypeMap().lookup(mallocType);
    assert(typeLayout != nullptr);
    // If we can't confirm it's a single object allocation, treat as array
    if (!isSingleAlloc(typeLayout, mallocSize))
      // TODO: adjust type layout when mallocSize is known (e.g. array count)
      typeLayout = TypeLayout::getByteArrayTypeLayout();
  }

  return evalMemoryAllocation(ctx, dstVal, typeLayout, true);
}

// Logic for Alloc effects.
void TransferFunction::evalExternalAlloc(
    const context::Context *ctx, const CallCFGNode &callNode,
    EvalResult &evalResult, const PointerAllocEffect &allocEffect,
    Store *&modifiedStore, bool &envChanged) {
  const auto *sizeVal =
      allocEffect.hasSizePosition()
          ? getArgument(callNode, allocEffect.getSizePosition())
          : nullptr;

  if (!allocEffect.getDestPosition().isReturnPosition()) {
    const auto *outArg = getArgument(callNode, allocEffect.getDestPosition());
    if (outArg != nullptr) {
      const auto *dstPtr =
          globalState.getPointerManager().getPointer(ctx, outArg);
      if (dstPtr != nullptr) {
        auto dstSet = globalState.getEnv().lookup(dstPtr);
        if (!dstSet.empty()) {
          const auto *mem = globalState.getMemoryManager().allocateHeapMemory(
              ctx, callNode.getCallSite(),
              TypeLayout::getByteArrayTypeLayout());
          if (modifiedStore == nullptr)
            modifiedStore = &evalResult.getNewStore(*localState);
          weakUpdateStore(dstSet, PtsSet::getSingletonSet(mem), *modifiedStore);
        }
      }
    }
    return;
  }

  // TODO: add type hint to malloc-like calls
  const auto *dstVal = callNode.getDest();
  if (dstVal != nullptr) {
    auto *mallocType = getMallocType(callNode.getCallSite());
    envChanged |= evalMallocWithSize(ctx, dstVal, mallocType, sizeVal);
  }
}

// Helper to simulate memcpy semantics on the store.
// Copies values from srcObjs to dstObj (and reachable objects).
void TransferFunction::evalMemcpyPtsSet(
    const MemoryObject *dstObj, llvm::ArrayRef<const MemoryObject *> srcObjs,
    size_t startingOffset, Store &store) {
  if (dstObj->isSpecialObject())
    return;
  auto &memManager = globalState.getMemoryManager();
  for (const auto *srcObj : srcObjs) {
    auto srcSet = store.lookup(srcObj);
    if (srcSet.empty())
      continue;

    // Calculate relative offset and find target sub-object
    auto offset = srcObj->getOffset() - startingOffset;
    const auto *tgtObj = memManager.offsetMemory(dstObj, offset);
    if (tgtObj->isSpecialObject())
      break;

    // Copy the points-to set (weak update because we are merging)
    store.weakUpdate(tgtObj, srcSet);
  }
}

// Resolves pointers for memcpy and iterates over source/dest objects.
bool TransferFunction::evalMemcpyPointer(const Pointer *dst, const Pointer *src,
                                         Store &store) {
  auto &env = globalState.getEnv();

  auto dstSet = env.lookup(dst);
  if (dstSet.empty())
    return false;
  auto srcSet = env.lookup(src);
  if (srcSet.empty())
    return false;

  auto &memManager = globalState.getMemoryManager();
  for (const auto *srcObj : srcSet) {
    if (srcObj->isNullObject())
      continue;
    if (srcObj->isUniversalObject()) {
      const auto uSet =
          PtsSet::getSingletonSet(MemoryManager::getUniversalObject());
      for (const auto *dstObj : dstSet) {
        if (dstObj->isSpecialObject())
          continue;
        for (const auto *tgtObj :
             memManager.getReachablePointerObjectsRef(dstObj))
          store.weakUpdate(tgtObj, uSet);
      }
      continue;
    }
    auto srcObjs = memManager.getReachablePointerObjectsRef(srcObj);
    for (const auto *dstObj : dstSet)
      evalMemcpyPtsSet(dstObj, srcObjs, srcObj->getOffset(), store);
  }
  return true;
}

// Entry point for memcpy effect.
bool TransferFunction::evalMemcpy(const context::Context *ctx,
                                  const CallCFGNode &callNode, Store &store,
                                  const APosition &dstPos,
                                  const APosition &srcPos) {
  assert(dstPos.isArgPosition() && srcPos.isArgPosition() &&
         "memcpy only operates on arguments");

  auto &ptrManager = globalState.getPointerManager();
  const auto *dstArg = getArgument(callNode, dstPos);
  const auto *srcArg = getArgument(callNode, srcPos);
  if (dstArg == nullptr || srcArg == nullptr)
    return false;
  const auto *dstPtr = ptrManager.getPointer(ctx, dstArg);
  if (dstPtr == nullptr)
    return false;
  const auto *srcPtr = ptrManager.getPointer(ctx, srcArg);
  if (srcPtr == nullptr)
    return false;

  return evalMemcpyPointer(dstPtr, srcPtr, store);
}

// Determines the points-to set for a Copy source.
PtsSet TransferFunction::evalExternalCopySource(const context::Context *ctx,
                                                const CallCFGNode &callNode,
                                                const CopySource &src) {
  switch (src.getType()) {
  case CopySource::SourceType::Value: {
    // Source is the value of the pointer argument itself (e.g., p = q)
    const auto *srcArg = getArgument(callNode, src.getPosition());
    if (srcArg == nullptr)
      return PtsSet::getEmptySet();
    const auto *ptr = globalState.getPointerManager().getPointer(ctx, srcArg);
    if (ptr == nullptr)
      return PtsSet::getEmptySet();
    return globalState.getEnv().lookup(ptr);
  }
  case CopySource::SourceType::DirectMemory: {
    // Source is the content of memory pointed to by argument (e.g., p = *q)
    const auto *srcArg = getArgument(callNode, src.getPosition());
    if (srcArg == nullptr)
      return PtsSet::getEmptySet();
    const auto *ptr = globalState.getPointerManager().getPointer(ctx, srcArg);
    if (ptr == nullptr)
      return PtsSet::getEmptySet();
    return loadFromPointer(ptr, *localState);
  }
  case CopySource::SourceType::Universal: {
    return PtsSet::getSingletonSet(MemoryManager::getUniversalObject());
  }
  case CopySource::SourceType::Null: {
    return PtsSet::getSingletonSet(MemoryManager::getNullObject());
  }
  case CopySource::SourceType::Static:
    // TODO: model "static" memory
    return PtsSet::getSingletonSet(MemoryManager::getUniversalObject());
  case CopySource::SourceType::ReachableMemory: {
    llvm_unreachable("ReachableMemory src should be handled earlier");
  }
  }
}

// Helper to fill a destination pointer's reachable memory with a source set.
// This is used when a function copies a value into *all* reachable sub-fields
// of a struct/array.
void TransferFunction::fillPtsSetWith(const Pointer *ptr, PtsSet srcSet,
                                      Store &store) {
  auto pSet = globalState.getEnv().lookup(ptr);

  for (const auto *obj : pSet) {
    if (obj->isSpecialObject())
      continue;

    for (const auto *tgtObj :
         globalState.getMemoryManager().getReachablePointerObjectsRef(obj))
      store.weakUpdate(tgtObj, srcSet);
  }
}

// Applies the copy result to the destination.
void TransferFunction::evalExternalCopyDest(const context::Context *ctx,
                                            const CallCFGNode &callNode,
                                            EvalResult &evalResult,
                                            const CopyDest &dest,
                                            PtsSet srcSet,
                                            Store *&modifiedStore,
                                            bool &envChanged) {
  // If the return value is not a pointer (e.g. void), skip writing it.
  if (callNode.getDest() == nullptr && dest.getPosition().isReturnPosition())
    return;

  const auto *dstArg = getArgument(callNode, dest.getPosition());
  if (dstArg == nullptr)
    return;

  const auto *dstPtr =
      globalState.getPointerManager().getOrCreatePointer(ctx, dstArg);
  switch (dest.getType()) {
  case CopyDest::DestType::Value: {
    envChanged |= globalState.getEnv().weakUpdate(dstPtr, srcSet);
    break;
  }
  case CopyDest::DestType::DirectMemory: {
    // Destination is memory pointed to by argument (e.g., *p = ...)
    auto dstSet = globalState.getEnv().lookup(dstPtr);
    if (dstSet.empty())
      return;

    if (modifiedStore == nullptr)
      modifiedStore = &evalResult.getNewStore(*localState);
    weakUpdateStore(dstSet, srcSet, *modifiedStore);
    break;
  }
  case CopyDest::DestType::ReachableMemory: {
    // Destination is all memory reachable from argument
    if (globalState.getEnv().lookup(dstPtr).empty())
      return;
    if (modifiedStore == nullptr)
      modifiedStore = &evalResult.getNewStore(*localState);
    fillPtsSetWith(dstPtr, srcSet, *modifiedStore);
    break;
  }
  }
}

// Dispatches copy effects (assignment, load, store, memcpy).
void TransferFunction::evalExternalCopy(const context::Context *ctx,
                                        const CallCFGNode &callNode,
                                        EvalResult &evalResult,
                                        const PointerCopyEffect &copyEffect,
                                        Store *&modifiedStore,
                                        bool &envChanged) {
  auto const &src = copyEffect.getSource();
  auto const &dest = copyEffect.getDest();

  // Special case for memcpy: the source is not a single ptr/mem
  if (src.getType() == CopySource::SourceType::ReachableMemory) {
    assert(dest.getType() == CopyDest::DestType::ReachableMemory &&
           "R src can only be assigned to R dest");

    Store &targetStore = modifiedStore != nullptr
                             ? *modifiedStore
                             : evalResult.getNewStore(*localState);
    if (evalMemcpy(ctx, callNode, targetStore, dest.getPosition(),
                   src.getPosition())) {
      modifiedStore = &targetStore;
    }
  } else {
    // General case: src is Value/DirectMemory/etc.
    auto srcSet = evalExternalCopySource(ctx, callNode, src);
    if (!srcSet.empty())
      evalExternalCopyDest(ctx, callNode, evalResult, dest, srcSet,
                           modifiedStore, envChanged);
  }
}

// Dispatches based on effect type (Alloc, Copy, Exit).
void TransferFunction::evalExternalCallByEffect(const context::Context *ctx,
                                                const CallCFGNode &callNode,
                                                const PointerEffect &effect,
                                                EvalResult &evalResult,
                                                Store *&modifiedStore,
                                                bool &envChanged,
                                                bool &hasExit) {
  switch (effect.getType()) {
  case PointerEffectType::Alloc: {
    evalExternalAlloc(ctx, callNode, evalResult, effect.getAsAllocEffect(),
                      modifiedStore, envChanged);
    break;
  }
  case PointerEffectType::Copy: {
    evalExternalCopy(ctx, callNode, evalResult, effect.getAsCopyEffect(),
                     modifiedStore, envChanged);
    break;
  }
  case PointerEffectType::Exit:
    // Exit effect: do not add any successors, terminating the path.
    hasExit = true;
    break;
  }
}

// Main handler for external calls.
// Checks intrinsic ID or looks up the ExternalPointerTable.
void TransferFunction::evalExternalCall(const context::Context *ctx,
                                        const CallCFGNode &callNode,
                                        const FunctionContext &fc,
                                        EvalResult &evalResult) {
  // Handle LLVM intrinsics that are relevant/ignorable
  if (fc.getFunction()->isIntrinsic()) {
    switch (fc.getFunction()->getIntrinsicID()) {
    case Intrinsic::dbg_value:
    case Intrinsic::dbg_declare:
    case Intrinsic::dbg_label:
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
      // These are no-ops for pointer analysis
      addMemLevelSuccessors(ProgramPoint(ctx, &callNode), *localState,
                            evalResult);
      return;
    default:
      break;
    }
  }

  // Look up annotations for library functions
  const auto *summary =
      globalState.getExternalPointerTable().lookup(fc.getFunction()->getName());
  if (summary == nullptr) {
    auto &ptrManager = globalState.getPointerManager();
    auto &env = globalState.getEnv();
    const auto *uObj = MemoryManager::getUniversalObject();
    const auto uSet = PtsSet::getSingletonSet(uObj);

    // Escape all pointer arguments: write Universal into their reachable
    // memory. Only create a new store copy if we actually need to modify it.
    Store *modifiedStore = nullptr;
    for (const auto *argVal : callNode) {
      const auto *argPtr = ptrManager.getPointer(ctx, argVal);
      if (argPtr == nullptr)
        continue;
      auto argPtsSet = env.lookup(argPtr);
      for (const auto *obj : argPtsSet) {
        if (obj->isSpecialObject())
          continue;
        // Lazily create the store copy on first modification.
        if (modifiedStore == nullptr)
          modifiedStore = &evalResult.getNewStore(*localState);
        // Weak-update all reachable pointer fields with Universal.
        for (const auto *rObj :
             globalState.getMemoryManager().getReachablePointerObjectsRef(obj))
          modifiedStore->weakUpdate(rObj, uSet);
      }
    }

    // If the call has a pointer-typed return destination, it may return
    // Universal.
    bool envChanged = false;
    if (const auto *dstVal = callNode.getDest()) {
      const auto *dstPtr = ptrManager.getOrCreatePointer(ctx, dstVal);
      envChanged = env.weakUpdate(dstPtr, uSet);
    }

    if (envChanged)
      addTopLevelSuccessors(ProgramPoint(ctx, &callNode), evalResult);
    addMemLevelSuccessors(
        ProgramPoint(ctx, &callNode),
        modifiedStore != nullptr ? *modifiedStore : *localState, evalResult);
    return;
  }

  // If the external func is a noop, we still need to propagate
  if (summary->empty()) {
    addMemLevelSuccessors(ProgramPoint(ctx, &callNode), *localState,
                          evalResult);
  } else {
    // Apply all recorded effects onto a single optional Store and propagate
    // memory/top-level successors once (avoiding dropped propagation on empty
    // copy sources / void returns and redundant duplicate Store merges).
    Store *modifiedStore = nullptr;
    bool envChanged = false;
    bool hasExit = false;
    for (auto const &effect : *summary)
      evalExternalCallByEffect(ctx, callNode, effect, evalResult, modifiedStore,
                               envChanged, hasExit);
    if (envChanged)
      addTopLevelSuccessors(ProgramPoint(ctx, &callNode), evalResult);
    if (!hasExit)
      addMemLevelSuccessors(
          ProgramPoint(ctx, &callNode),
          modifiedStore != nullptr ? *modifiedStore : *localState, evalResult);
  }
}

} // namespace tpa