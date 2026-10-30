#pragma once

#include <cstdint>
#include <limits>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Operator.h>

namespace lotus::analysis {
// Collapse array indices while preserving struct member offsets. Instructions
// and constant expressions must use the same abstract address calculation.
struct NormalizedGepOffset {
  uint64_t offset = 0;
  bool traversesArray = false;
  bool valid = false;
};

inline NormalizedGepOffset
getNormalizedGepOffset(const llvm::GEPOperator *gep,
                       const llvm::DataLayout &layout) {
  using namespace llvm;
  NormalizedGepOffset result;
  if (!gep)
    return result;

  Type *current = gep->getSourceElementType();
  bool firstIndex = true;
  for (const Use &indexUse : gep->indices()) {
    const auto *index = dyn_cast<ConstantInt>(indexUse.get());
    if (firstIndex) {
      firstIndex = false;
      if (!index) {
        // A symbolic byte/pointer stride cannot be represented as a known
        // member offset. Array/struct strides can still preserve members.
        if (current->isSingleValueType())
          return result;
        result.traversesArray = true;
      } else if (!current->isArrayTy() && !index->isZero()) {
        if (index->isNegative())
          return result;
        const uint64_t elementSize = layout.getTypeAllocSize(current);
        const uint64_t element = index->getZExtValue();
        if (elementSize != 0 &&
            element > std::numeric_limits<uint64_t>::max() / elementSize)
          return result;
        const uint64_t increment = element * elementSize;
        if (increment > std::numeric_limits<uint64_t>::max() - result.offset)
          return result;
        result.offset += increment;
      }
      continue;
    }
    if (auto *structure = dyn_cast<StructType>(current)) {
      if (!index || index->getZExtValue() >= structure->getNumElements())
        return result;
      const unsigned field = static_cast<unsigned>(index->getZExtValue());
      const uint64_t increment =
          layout.getStructLayout(structure)->getElementOffset(field);
      if (increment > std::numeric_limits<uint64_t>::max() - result.offset)
        return result;
      result.offset += increment;
      current = structure->getElementType(field);
      continue;
    }
    if (auto *array = dyn_cast<ArrayType>(current)) {
      result.traversesArray = true;
      current = array->getElementType();
      continue;
    }
    if (auto *vector = dyn_cast<VectorType>(current)) {
      result.traversesArray = true;
      current = vector->getElementType();
      continue;
    }
    return result;
  }
  result.valid = true;
  return result;
}

} // namespace lotus::analysis
