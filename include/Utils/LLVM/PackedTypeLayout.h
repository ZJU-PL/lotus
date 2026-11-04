#pragma once

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>

namespace lotus {

/// Bit widths for analyses that concatenate aggregate fields without padding.
/// This is an analysis abstraction, not a replacement for LLVM's ABI layout.
class PackedTypeLayout {
  uint64_t pointer_width_;

public:
  explicit PackedTypeLayout(const llvm::DataLayout &layout)
      : pointer_width_(layout.getPointerSizeInBits()) {}

  uint64_t getTypeSizeInBits(llvm::Type *type) const {
    if (type->isPointerTy())
      return pointer_width_;
    if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type))
      return array->getNumElements() * getTypeSizeInBits(array->getElementType());
    if (auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(type))
      return vector->getNumElements() *
             getTypeSizeInBits(vector->getElementType());
    if (auto *structure = llvm::dyn_cast<llvm::StructType>(type)) {
      uint64_t width = 0;
      for (llvm::Type *element : structure->elements())
        width += getTypeSizeInBits(element);
      return width;
    }
    if (type->isVectorTy())
      return 0; // Scalable vectors have no fixed bit-vector representation.
    return type->getPrimitiveSizeInBits().getFixedValue();
  }

  uint64_t getElementOffsetInBits(llvm::StructType *type, unsigned index) const {
    assert(index < type->getNumElements());
    uint64_t offset = 0;
    for (unsigned field = 0; field < index; ++field)
      offset += getTypeSizeInBits(type->getElementType(field));
    return offset;
  }

  unsigned getElementContainingOffset(llvm::StructType *type,
                                      uint64_t offset) const {
    uint64_t end = 0;
    for (unsigned field = 0; field < type->getNumElements(); ++field) {
      if (offset == end)
        return field;
      end += getTypeSizeInBits(type->getElementType(field));
      if (offset < end)
        return field;
    }
    return offset == end ? type->getNumElements() : 0;
  }
};

} // namespace lotus
