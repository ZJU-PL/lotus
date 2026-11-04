//
// Created by peiming on 12/18/19.
//

#pragma once

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/Layout/ArrayLayout.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/Layout/MemLayout.h"
#include "Alias/InclusionBased/AserPTA/Util/Log.h"

#include <unordered_map>

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Allocator.h>

namespace aser {

class MemLayoutManager {
private:
  using MemLayoutAllocator = llvm::SpecificBumpPtrAllocator<MemLayout>;
  using ArrayLayoutAllocator = llvm::SpecificBumpPtrAllocator<ArrayLayout>;
  // owner of memlayout
  MemLayoutAllocator memLayoutAllocator;
  ArrayLayoutAllocator arrayLayoutAllocator;

  // std::unordered_map<llvm::Type *, MemLayout *> memLayoutMap;
  llvm::DenseMap<llvm::Type *, MemLayout *> memLayoutMap;

  void setElementLayout(llvm::Type *elementType, MemLayout *parentLayout,
                        const llvm::DataLayout &DL, size_t &lOffset,
                        size_t &pOffset) {
    size_t typeAllocSize = DL.getTypeAllocSize(elementType);

    switch (elementType->getTypeID()) {
    case llvm::Type::StructTyID:
    case llvm::Type::ArrayTyID: {
      // recursive call, should be fine, the type tree shouldn't be too deep
      // change it if it becomes a problem.
      size_t subPOffset = 0, subLOffset = 0;
      const MemLayout *subLayout =
          DFSTypeTree(elementType, DL, subLOffset, subPOffset);
      parentLayout->mergeMemoryLayout(subLayout, pOffset, lOffset);
      lOffset += subLOffset;
      break;
    }
    case llvm::Type::PointerTyID:
      // if it is pointer, mark the offset
      parentLayout->setElementOffset(lOffset);
      parentLayout->setPointerOffset(lOffset);
      lOffset += typeAllocSize;
      break;
    case llvm::Type::FixedVectorTyID:
    case llvm::Type::ScalableVectorTyID: {
      // simply skip it
      LOG_TRACE("Unhandled Type. type={}", *elementType);
    }
    default: {
      // primitive type
      parentLayout->setElementOffset(lOffset);
      lOffset += typeAllocSize;
      break;
    }
    }
    // forward the physical offset
    pOffset += typeAllocSize;
  }

  const MemLayout *getArrayLayout(llvm::ArrayType *T,
                                  const llvm::DataLayout &DL, size_t &lOffset,
                                  size_t &pOffset) {
    assert(lOffset == 0 && pOffset == 0);

    auto *layout = new (memLayoutAllocator.Allocate()) MemLayout(true);
    // creating a new array layout
    llvm::Type *elementType = T->getElementType();
    size_t numElement = T->getArrayNumElements();
    size_t elementSize = DL.getTypeAllocSize(elementType);

    auto arrayLayout = new (arrayLayoutAllocator.Allocate())
        ArrayLayout(numElement, elementSize);
    layout->insertSubArray(0, arrayLayout);

    // set the element layout
    setElementLayout(elementType, layout, DL, lOffset, pOffset);
    // forward the physical offset
    if (numElement != std::numeric_limits<size_t>::max()) {
      pOffset += elementSize * (numElement - 1);
    } else {
      pOffset = std::numeric_limits<size_t>::max();
    }
    // layout->insertArrayTuple(numElement, elementSize, 0, layoutOffset);

    if (numElement != std::numeric_limits<size_t>::max()) {
      assert(pOffset == DL.getTypeAllocSize(T));
    }

    layout->setMaxOffsets(lOffset, pOffset);
    arrayLayout->setLayoutSize(lOffset);

    // cache the type layout
    bool result = memLayoutMap.insert(std::make_pair(T, layout)).second;
    assert(result && "creating a exsiting type layout");
    return layout;
  }

  const MemLayout *getStructLayout(llvm::StructType *T,
                                   const llvm::DataLayout &DL, size_t &lOffset,
                                   size_t &pOffset) {
    assert(lOffset == 0 && pOffset == 0);
    // create a new MemLayout
    auto *layout = new (memLayoutAllocator.Allocate()) MemLayout;
    auto structLayout = DL.getStructLayout(T);

    for (unsigned i = 0; i < T->getNumElements(); i++) {
      // accumulate physical offset
      size_t elemOffset = structLayout->getElementOffset(i);
      assert(elemOffset >= pOffset);

      // get the memory layout of sub elements.
      auto elementType = T->getElementType(i);
      size_t padding = elemOffset - pOffset;
      // adjust the padding in the structure
      lOffset += padding;
      pOffset += padding;

      // recursively build the sub-layout of the field
      this->setElementLayout(elementType, layout, DL, lOffset, pOffset);
    }

    // here pOffset might not equal to the allocation size because of the
    // padding. the memory beyond the last element is used as padding, adjust
    // the layout accordingly
    if (pOffset != DL.getTypeAllocSize(T)) {
      assert(pOffset < DL.getTypeAllocSize(T));

      size_t padding = DL.getTypeAllocSize(T) - pOffset;
      pOffset += padding;
      lOffset += padding;
    }
    // assert(structLayout->hasPadding() ? true: pOffset ==
    // DL.getTypeAllocSize(T));
    layout->setMaxOffsets(lOffset, pOffset);
    // cached the result
    bool result = memLayoutMap.insert(std::make_pair(T, layout)).second;
    assert(result && "creating a existing type layout");
    return layout;
  }

  // TODO: change it to non-recursive version if the type tree can be too deep
  // layout offset may not be equal to physical offset at the presence of
  // arrays.
  const MemLayout *DFSTypeTree(llvm::Type *T, const llvm::DataLayout &DL,
                               size_t &lOffset, size_t &pOffset) {
    assert(T->isAggregateType());
    // already cached
    auto it = memLayoutMap.find(T);
    if (it != memLayoutMap.end()) {
      // also update the layout offset and physical offset
      const MemLayout *layout = it->second;
      lOffset += layout->maxLOffset;
      pOffset += layout->maxPOffset;
      return layout;
    }

    // dispatch
    if (auto arrayTy = llvm::dyn_cast<llvm::ArrayType>(T)) {
      return getArrayLayout(arrayTy, DL, lOffset, pOffset);
    } else if (auto structTy = llvm::dyn_cast<llvm::StructType>(T)) {
      return getStructLayout(structTy, DL, lOffset, pOffset);
    }

    llvm_unreachable("aggregate type is not array or structure!?");
  }

public:
  const MemLayout *getLayoutForType(llvm::Type *T, const llvm::DataLayout &DL) {
    // offset index the layout table might be different than the phsical layout
    // of the type as arrays are collapsed.
    size_t lOffset = 0, pOffset = 0;
    return DFSTypeTree(T, DL, lOffset, pOffset);
  }
};

} // namespace aser

