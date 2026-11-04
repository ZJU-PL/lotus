//
// Created by peiming on 12/18/19.
//
#pragma once

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Graph/ConstraintGraph/ConstraintGraph.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSCanonicalizer.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSObject.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/Layout/MemLayoutManager.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/MemBlock.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/InclusionBased/AserPTA/Util/ConstExprVisitor.h"
#include "Alias/InclusionBased/AserPTA/Util/Log.h"

#include <llvm/Support/Allocator.h>
#include <glob.h>

namespace aser {

size_t getGEPStepSize(const llvm::GetElementPtrInst *GEP,
                      const llvm::DataLayout &DL);

template <typename ctx> class FSMemModel {
  using CT = CtxTrait<ctx>;
  using Self = FSMemModel<ctx>;
  using ObjNode = CGObjNode<Self>;
  using PtrNode = CGPtrNode<ctx>;
  using BaseNode = CGNodeBase<ctx>;
  using ConsGraphTy = ConstraintGraph<ctx>;
  using PtrOwner = SingleInstanceOwner<Pointer<ctx>>;
  using Canonicalizer = FSCanonicalizer;
  using MemBlockAllocator = llvm::BumpPtrAllocator;
  using CtxAllocPair = std::pair<const ctx *, const llvm::Value *>;
  MemBlockAllocator Allocator;

  const PtrOwner &ptrOwner; // the manager for pointer
  ConsGraphTy &consGraph;

  ObjNode *nullObjNode = nullptr;
  ObjNode *uniObjNode = nullptr;

  MemLayoutManager layoutManager;
  // map from a allocation site to the Memory Block
  llvm::DenseMap<CtxAllocPair, MemBlock<ctx> *> memBlockMap;

  template <typename PT> inline ObjNode *createNode(const FSObject<ctx> *obj) {
    assert(obj != nullptr);
    auto ret = consGraph.template addCGNode<ObjNode, PT>(obj);
    const_cast<FSObject<ctx> *>(obj)->setObjNode(ret);
    return ret;
  }

  inline MemBlock<ctx> *getMemBlock(const ctx *c, const llvm::Value *v) {
    // v is the allocation site of the memory block
    if (llvm::isa<llvm::GlobalVariable>(v)) {
      c = CT::getGlobalCtx();
    }
    auto it = memBlockMap.find(std::make_pair(c, v));
    if (it == memBlockMap.end()) {
      // This can happen for external / declaration-only globals or
      // constants we do not model. Be conservative and return nullptr.
      LOG_DEBUG("FSMemModel: cannot find memory block for value {}", *v);
      return nullptr;
    }
    return it->second;
  }

  template <typename BlockT, typename... Args>
  MemBlock<ctx> *allocMemBlock(const ctx *c, const llvm::Value *v,
                               Args &&...args) {
    bool result;
    MemBlock<ctx> *block =
        new (Allocator) BlockT(c, v, std::forward<Args>(args)...);
    // we do not to put anonymous object into the map
    if (block->getAllocSite().getAllocType() != AllocType::Anonymous) {
      std::tie(std::ignore, result) =
          this->memBlockMap.insert(std::make_pair(std::make_pair(c, v), block));
      // must be adding a new memory block
      assert(result && "allocating a existing memory block");
    }
    return block;
  }

  // The llvm::type does not have to be consistent with the type of the
  // llvm::value as the heap allocation type might be inferred using heuristics.
  template <typename PT>
  inline ObjNode *allocValueWithType(const ctx *C, const llvm::Value *V,
                                     AllocType T, llvm::Type *type,
                                     const llvm::DataLayout &DL) {
    assert(type && "llvm::Type can not be null");
    LOG_TRACE("allocate object. type={}", *type);

    MemBlock<ctx> *block;
    switch (type->getTypeID()) {
    case llvm::Type::StructTyID:
    case llvm::Type::ArrayTyID: {
      if (auto *ST = llvm::dyn_cast<llvm::StructType>(type)) {
        if (ST->isOpaque()) {
          // do not know the type layout for a opaque type,
          // simply treat it as field-insensitive object
          LOG_DEBUG("Value has opaque type. value={}", *V);
          block = this->allocMemBlock<FIMemBlock<ctx>>(C, V, T);
          break;
        }
      }
      auto layout = layoutManager.getLayoutForType(type, DL);
      block = this->allocMemBlock<AggregateMemBlock<ctx>>(C, V, T, layout);
      break;
    }
    case llvm::Type::FixedVectorTyID:
    case llvm::Type::ScalableVectorTyID: {
      LOG_DEBUG("Vector Type not handled. type={}", *V);
      block = this->allocMemBlock<FIMemBlock<ctx>>(C, V, T);
      break;
      // llvm_unreachable("vector type not handled");
    }
    default: {
      // TODO: when will i8* be a FIObject?
      block = this->allocMemBlock<ScalarMemBlock<ctx>>(C, V, T);
      break;
    }
    }

    assert(block && "MemBlock can not be NULL!");
    // get the object at 0 offset
    return createNode<PT>(block->getObjectAt(0));
  }

public:
  FSMemModel(ConsGraphTy &consGraph, const PtrOwner &owner)
      : ptrOwner(owner), consGraph(consGraph) {}

private:
  template <typename PT>
  inline ObjNode *allocNullObj(const llvm::Module *module) {
    assert(!nullObjNode && "recreating a null object!");

    auto *v = llvm::ConstantPointerNull::get(
        llvm::Type::getInt8PtrTy(module->getContext()));
    auto *block = this->allocMemBlock<FIMemBlock<ctx>>(CT::getGlobalCtx(), v,
                                                       AllocType::Null);
    return this->template createNode<PT>(block->getObjectAt(0));
  }

  template <typename PT>
  inline ObjNode *allocUniObj(const llvm::Module *module) {
    assert(!uniObjNode && "recreating a universal object!");

    auto *v =
        llvm::UndefValue::get(llvm::Type::getInt8PtrTy(module->getContext()));
    auto *block = this->allocMemBlock<FIMemBlock<ctx>>(CT::getGlobalCtx(), v,
                                                       AllocType::Universal);
    return this->template createNode<PT>(block->getObjectAt(0));
  }

  template <typename PT>
  inline ObjNode *allocGlobalVariable(const llvm::GlobalVariable *g,
                                      const llvm::DataLayout &DL) {
    LOG_TRACE("allocating global. global={}", *g);

    auto *pointedType = g->getType()->getPointerElementType();
    return this->allocValueWithType<PT>(CT::getGlobalCtx(), g,
                                        AllocType::Globals, pointedType, DL);
  }

  template <typename PT>
  inline void handleMemCpy(const ctx *C, const llvm::MemCpyInst *memCpy,
                           PtrNode *src, PtrNode *dst) {
    // TODO: memcpy whose cpy size is constant should already be lowered before
    // PTA, for the remaining memcpy, we do not handle it for now. if we handle
    // it, it more introduce too many false positive, as we have to do it
    // conservatively
    LOG_TRACE("unhandled memcpy instruction. inst={}", *memCpy);
  }

  template <typename PT>
  inline ObjNode *allocStackObj(const ctx *C, const llvm::AllocaInst *I,
                                const llvm::DataLayout &DL) {
    LOG_TRACE("allocating Stack Object. inst={}", *I);

    const llvm::Value *arraySize = I->getArraySize();
    auto *elementType = I->getType()->getPointerElementType();

    llvm::Type *type;
    if (const auto *constSize = llvm::dyn_cast<llvm::ConstantInt>(arraySize)) {
      size_t elementNum = constSize->getSExtValue();
      if (elementNum == 1) {
        type = elementType;
      } else {
        type = llvm::ArrayType::get(elementType, elementNum);
      }
    } else {
      type =
          llvm::ArrayType::get(elementType, std::numeric_limits<size_t>::max());
    }

    return this->allocValueWithType<PT>(C, I, AllocType::Stack, type, DL);
  }

  template <typename PT>
  inline ObjNode *allocFunction(const llvm::Function *f) {
    // create a function object (function pointer can not be indexed as well)
    MemBlock<ctx> *block = this->allocMemBlock<ScalarMemBlock<ctx>>(
        CT::getGlobalCtx(), f, AllocType::Functions);
    return createNode<PT>(block->getObjectAt(0));
  }

  template <typename PT>
  inline ObjNode *allocHeapObj(const ctx *C, const llvm::Instruction *I,
                               const llvm::DataLayout &DL, llvm::Type *T) {
    if (T != nullptr) {
      return this->allocValueWithType<PT>(C, I, AllocType::Heap, T, DL);
    }

    // we can not infer the type of the allocating heap object,
    // conservatively treat it field insensitively.
    MemBlock<ctx> *block =
        this->allocMemBlock<FIMemBlock<ctx>>(C, I, AllocType::Heap);
    return createNode<PT>(block->getObjectAt(0));
  }

  template <typename PT>
  void initAnonAggregateObj(
      const ctx *C, const llvm::DataLayout &DL, llvm::Type *T,
      MemBlock<ctx> *block, std::vector<const llvm::Type *> &typeTree,
      const llvm::Value *tag,
      size_t &globOffset) { // the offset of the field currently being accessed

    // flatten the array/structure type
    auto *structTy = llvm::cast<llvm::StructType>(T);
    const auto *layout = DL.getStructLayout(structTy);

    for (unsigned i = 0; i < structTy->getNumElements(); i++) {
      auto *elemTy = structTy->getElementType(i);

      if (llvm::isa<llvm::ArrayType>(elemTy)) {
        // strip all the array to get the inner most type
        do {
          elemTy = elemTy->getArrayElementType();
        } while (llvm::isa<llvm::ArrayType>(elemTy));
      }

      size_t offset = globOffset + layout->getElementOffset(i);
      if (auto *structElem = llvm::dyn_cast<llvm::StructType>(elemTy)) {
        // recursive into the type tree.
        // since the type tree is normally not so deep, probably okay
        initAnonAggregateObj<PT>(C, DL, elemTy, block, typeTree, tag, offset);
      } else if (auto *ptrElem = llvm::dyn_cast<llvm::PointerType>(elemTy)) {
        // skipping the element type if the type is not valid for array
        // e.g., function, void type
        ObjNode *child = allocAnonObjRec<PT>(
            C, DL, getUnboundedArrayTy(elemTy->getPointerElementType()), tag,
            typeTree);
        if (child != nullptr) {
          const FSObject<ctx> *object = block->getPtrObjectAt(offset);
          assert(object && "No Pointer Element at the offset");
          if (object->getObjNodeOrNull() == nullptr) {
            createNode<PT>(object);
          } else {
            assert(offset == 0);
          }

          consGraph.addConstraints(child, object->getObjNode(),
                                   Constraints::addr_of);
        }
      }

      // skip other type
    }

    // accumulated the global offset
    globOffset += DL.getTypeAllocSize(structTy);
  }

  template <typename PT>
  ObjNode *allocAnonObjRec(const ctx *C, const llvm::DataLayout &DL,
                           llvm::Type *T, const llvm::Value *tag,
                           std::vector<const llvm::Type *> &typeTree) {
    if (std::find(typeTree.begin(), typeTree.end(), T) != typeTree.end() ||
        T == nullptr) {
      // recursive type
      // i.e., link_list {link_list *next};
      return nullptr;
    }

    // DFS over the type tree
    typeTree.push_back(T);
    ObjNode *objNode =
        this->allocValueWithType<PT>(C, tag, AllocType::Anonymous, T, DL);
    MemBlock<ctx> *block = objNode->getObject()->memBlock;

    // first allocate the root memoryblock and create the memo
    switch (T->getTypeID()) {
    case llvm::Type::StructTyID: {
      // flatten the aggregated type and allocate their child
      // result = allocAnonAggregateObj(C, DL, T, typeTree);
      size_t offset = 0;
      initAnonAggregateObj<PT>(C, DL, T, block, typeTree, tag, offset);
      break;
    }
    case llvm::Type::ArrayTyID: {
      llvm::Type *elemTy = T;
      do {
        // strip all the array to get the inner most type
        elemTy = elemTy->getArrayElementType();
      } while (llvm::isa<llvm::ArrayType>(elemTy));
      assert(!llvm::isa<llvm::ArrayType>(elemTy));

      if (auto *structElem = llvm::dyn_cast<llvm::StructType>(elemTy)) {
        size_t offset = 0;
        // the element is a structure, try to initialize it recursively
        initAnonAggregateObj<PT>(C, DL, elemTy, block, typeTree, tag, offset);
      } else if (auto *ptrElem = llvm::dyn_cast<llvm::PointerType>(elemTy)) {
        // treat ptr as array to allow indexing, that is int * -> int []
        ObjNode *child = allocAnonObjRec<PT>(
            C, DL, getUnboundedArrayTy(elemTy->getPointerElementType()), tag,
            typeTree);
        if (child != nullptr) {
          consGraph.addConstraints(child, objNode, Constraints::addr_of);
        }
      }
      break;
    }
    case llvm::Type::PointerTyID: {
      ObjNode *child = allocAnonObjRec<PT>(
          C, DL, getUnboundedArrayTy(T->getPointerElementType()), tag,
          typeTree);
      if (child != nullptr) {
        consGraph.addConstraints(child, objNode, Constraints::addr_of);
      }
      break;
    }
    default:
      break;
    }

    // pop the type tree
    typeTree.pop_back();
    return objNode;
  }

  template <typename PT>
  inline ObjNode *allocAnonObj(const ctx *C, const llvm::DataLayout &DL,
                               llvm::Type *T, const llvm::Value *tag,
                               bool recursive) {
    // the anonymous object can not be access and looked up
    if (T != nullptr) {
      if (recursive) {
        // if we are request to build the object recursively...
        // initialize it according to the type tree
        std::vector<const llvm::Type *> DFSStack;
        return allocAnonObjRec<PT>(C, DL, T, tag, DFSStack);
      }
      return this->allocValueWithType<PT>(C, tag, AllocType::Anonymous, T, DL);
      ;
    }

    MemBlock<ctx> *block =
        this->allocMemBlock<FIMemBlock<ctx>>(C, tag, AllocType::Anonymous);
    return createNode<PT>(block->getObjectAt(0));
  }

  template <typename PT>
  inline void processScalarGlobals(MemBlock<ctx> *memBlock,
                                   const llvm::Constant *C, size_t &offset,
                                   const llvm::DataLayout &DL) {
    if (C->getType()->isPointerTy() && !llvm::isa<llvm::BlockAddress>(C)) {
      /* FIXME: what if the constexpr is a complicated expression or global
         alias? need to evaluate it statically. */
      // now assume it point is a global variable
      // llvm::APInt constOffset(64, 0);
      C = llvm::dyn_cast<llvm::Constant>(Canonicalizer::canonicalize(C));
      // auto V = C->stripAndAccumulateConstantOffsets(DL, constOffset, true);

      auto *ptr = memBlock->getObjectAt(offset);

      // The offset might fall into padding or into a region that is not
      // indexable according to our memory layout. In that case we do not
      // create any constraints but still advance the offset.
      if (ptr == nullptr) {
        offset += DL.getTypeAllocSize(C->getType());
        return;
      }

      if (ptr->getObjNodeOrNull() == nullptr) {
        createNode<PT>(ptr);
      }

      auto *ptrNode = ptr->getObjNode();
      ObjNode *objNode = nullptr;
      if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(C)) {
        auto off = llvm::APInt(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
        const auto *baseValue = GEP->stripAndAccumulateConstantOffsets(DL, off, true);
        // objNode can be none, when it is a external symbol, which does not
        // have initializers.
        if (baseValue != nullptr) {
          if (auto *baseBlock = getMemBlock(CT::getGlobalCtx(), baseValue)) {
            auto FSobj = baseBlock->getObjectAt(off.getSExtValue());
            // FIXME: Field-sensitive object can be nullptr, because we handle
            // i8* as scalar object however, it should be the most conservative
            // type in LLVM (void *) probably should handle it as a
            // field-insensitive object.
            if (FSobj != nullptr) {
              // the obj node can also be null because of external object
              // TODO: figure out whether the order of the global variables'
              // definition is the same as the def-use order.
              objNode = FSobj->getObjNodeOrNull();
            }
          }
        }
      } else {
        if (auto *baseBlock = getMemBlock(CT::getGlobalCtx(), C)) {
          if (auto *baseObj = baseBlock->getObjectAt(0)) {
            objNode = baseObj->getObjNodeOrNull();
          }
        }
      }

      if (objNode != nullptr) {
        consGraph.addConstraints(objNode, ptrNode, Constraints::addr_of);
      }
    }

    // accumulate the physical offset
    offset += DL.getTypeAllocSize(C->getType());
  }

  template <typename PT>
  inline void processAggregateGlobals(MemBlock<ctx> *memBlock,
                                      const llvm::Constant *C, size_t &offset,
                                      const llvm::DataLayout &DL) {
    assert(llvm::isa<llvm::ConstantArray>(C) ||
           llvm::isa<llvm::ConstantStruct>(C) ||
           llvm::isa<llvm::ConstantDataArray>(C));

    size_t initOffset = offset;
    if (llvm::isa<llvm::ConstantArray>(C) ||
        llvm::isa<llvm::ConstantStruct>(C)) {
      for (unsigned i = 0, e = C->getNumOperands(); i != e; ++i) {
        // make up the padding if it is a structure type.
        if (auto *structType = llvm::dyn_cast<llvm::StructType>(C->getType())) {
          const auto *structLayout = DL.getStructLayout(structType);
          if (structLayout->hasPadding()) {
            assert(offset >= initOffset);
            size_t padding =
                structLayout->getElementOffset(i) - (offset - initOffset);
            offset += padding;
          }
        }

        // recursively traverse the initializer
        processInitializer<PT>(
            memBlock, llvm::cast<llvm::Constant>(C->getOperand(i)), offset, DL);
      }
      // make up the padding if it is a struct type
      if (auto *structType = llvm::dyn_cast<llvm::StructType>(C->getType())) {
        // if (initOffset + DL.getTypeAllocSize(C->getType()) - offset != 0) {
        offset = initOffset + DL.getTypeAllocSize(C->getType());
        //}
      }
    } else if (llvm::isa<llvm::ConstantDataArray>(C)) {
      // Constant Data Array does not have operand, as they store
      // the value directly instead of as Value*s
      // it does not contains pointers, simply skip it.
      // For something like:
      // private unnamed_addr constant [8 x i8] c"abcdefg\00", align 1
      offset += DL.getTypeAllocSize(C->getType());
    }
  }

  // TODO: change it to non-recursive version if the initializer is deep
  template <typename PT>
  void processInitializer(MemBlock<ctx> *memBlock,
                          const llvm::Constant *initializer, size_t &offset,
                          const llvm::DataLayout &DL) {
    if (initializer->isNullValue() ||
        llvm::isa<llvm::UndefValue>(initializer)) {
      // skip zero initializer and undef initializer
      offset += DL.getTypeAllocSize(initializer->getType());
    } else if (initializer->getType()->isSingleValueType()) {
      processScalarGlobals<PT>(memBlock, initializer, offset, DL);
    } else {
      processAggregateGlobals<PT>(memBlock, initializer, offset, DL);
    }
  }

  template <typename PT>
  void initializeGlobal(const llvm::GlobalVariable *gVar,
                        const llvm::DataLayout &DL) {
    if (gVar->hasInitializer()) {
      size_t offset = 0;

      // 1st, get the memory block of the global variable
      MemBlock<ctx> *memBlock = getMemBlock(CT::getGlobalCtx(), gVar);
      if (memBlock != nullptr) {
        // 2nd, recursively initialize the global variable
        processInitializer<PT>(memBlock, gVar->getInitializer(), offset, DL);
      } else {
        // For declaration-only globals or ones we have not modeled, we
        // conservatively skip initialization to avoid crashing.
        LOG_DEBUG("FSMemModel: skip initializing global without MemBlock: {}",
                  *gVar);
      }
    }
    // else an extern symbol, conservatively can point to anything, simply skip
    // it for now
  }

  template <typename PT>
  ObjNode *indexObject(const FSObject<ctx> *obj,
                       const llvm::GetElementPtrInst *gep) {
    const llvm::DataLayout &DL = gep->getModule()->getDataLayout();

    // cache object indexing for performance
    // TODO: Does it worth?
    // std::pair<ObjNode *, bool> result = obj->getCachedResult(gep);

    // if (result.second) {
    //     return result.first;
    // }

    if (gep->hasAllConstantIndices()) {
      // simple case, indexing object using constant offset.
      auto offset = llvm::APInt(DL.getIndexTypeSizeInBits(gep->getType()), 0);
      gep->accumulateConstantOffset(DL, offset);

      auto result =
          obj->memBlock->getObjectAt(obj->pOffset + offset.getSExtValue());
      if (result == nullptr) {
        // obj->cacheIndexResult(gep, nullptr);
        return nullptr;
      }

      if (result->getObjNodeOrNull() == nullptr) {
        createNode<PT>(result);
      }

      // obj->cacheIndexResult(gep, result->getObjNode());
      return result->getObjNode();
    } else {
      size_t stepSize = getGEPStepSize(gep, DL);

      const FSObject<ctx> *result = obj->getObjIndexedByVar(stepSize);
      if (result == nullptr) {
        // obj->cacheIndexResult(gep, nullptr);
        return nullptr;
      }

      assert(result->getObjNodeOrNull() != nullptr);
      // obj->cacheIndexResult(gep, result->getObjNode());
      return result->getObjNode();
    }
  }

  friend MemModelTrait<FSMemModel<ctx>>;
};

template <typename ctx> struct MemModelTrait<FSMemModel<ctx>> {
  using CtxTy = ctx;
  using ObjectTy = FSObject<ctx>;
  using Canonicalizer = FSCanonicalizer;

  // whether *all* GEPs will be collapse
  static const bool COLLAPSE_GEP = false;
  // whether all BitCast will be collapse
  static const bool COLLAPSE_BITCAST = true;
  // whether type information is necessary
  // we need type information to build the memory layout
  static const bool NEED_TYPE_INFO = true;

  // add required passes into pass manager
  static inline void addDependentPasses(llvm::legacy::PassManager &passes) {
    // passes.add(new CanonicalizeInstPass(true));
    // passes.add(new LoweringMemCpyPass());
  }

  template <typename PT>
  inline static CGObjNode<FSMemModel<ctx>> *
  allocateNullObj(FSMemModel<ctx> &model, const llvm::Module *module) {
    return model.template allocNullObj<PT>(module);
  }

  template <typename PT>
  inline static CGObjNode<FSMemModel<ctx>> *
  allocateUniObj(FSMemModel<ctx> &model, const llvm::Module *module) {
    return model.template allocUniObj<PT>(module);
  }

  template <typename PT>
  inline static CGObjNode<FSMemModel<ctx>> *
  allocateFunction(FSMemModel<ctx> &model, const llvm::Function *fun) {
    return model.template allocFunction<PT>(fun);
  }

  template <typename PT>
  inline static CGObjNode<FSMemModel<ctx>> *
  allocateGlobalVariable(FSMemModel<ctx> &model,
                         const llvm::GlobalVariable *gVar,
                         const llvm::DataLayout &DL) {
    return model.template allocGlobalVariable<PT>(gVar, DL);
  }

  template <typename PT>
  inline static CGObjNode<FSMemModel<ctx>> *
  allocateStackObj(FSMemModel<ctx> &model, const ctx *context,
                   const llvm::AllocaInst *gVar, const llvm::DataLayout &DL) {
    return model.template allocStackObj<PT>(context, gVar, DL);
  }

  template <typename PT>
  inline static CGObjNode<FSMemModel<ctx>> *
  allocateHeapObj(FSMemModel<ctx> &model, const ctx *context,
                  const llvm::Instruction *callsite, const llvm::DataLayout &DL,
                  llvm::Type *T) {
    return model.template allocHeapObj<PT>(context, callsite, DL, T);
  }

  template <typename PT>
  inline static CGObjNode<FSMemModel<ctx>> *
  allocateAnonObj(FSMemModel<ctx> &model, const ctx *context,
                  const llvm::DataLayout &DL, llvm::Type *T,
                  const llvm::Value *tag = nullptr, bool recursive = true) {
    return model.template allocAnonObj<PT>(context, DL, T, tag, recursive);
  }

  template <typename PT>
  inline static CGObjNode<FSMemModel<ctx>> *
  indexObject(FSMemModel<ctx> &model, const FSObject<ctx> *obj,
              const llvm::GetElementPtrInst *gep) {
    return model.template indexObject<PT>(obj, gep);
  }

  template <typename PT>
  inline static void handleMemCpy(FSMemModel<ctx> &model, const ctx *C,
                                  const llvm::MemCpyInst *memCpy,
                                  CGPtrNode<ctx> *src, CGPtrNode<ctx> *dst) {
    return model.template handleMemCpy<PT>(C, memCpy, src, dst);
  }

  template <typename PT>
  inline static void initializeGlobal(FSMemModel<ctx> &memModel,
                                      const llvm::GlobalVariable *gVar,
                                      const llvm::DataLayout &DL) {
    memModel.template initializeGlobal<PT>(gVar, DL);
  }
};

} // namespace aser
