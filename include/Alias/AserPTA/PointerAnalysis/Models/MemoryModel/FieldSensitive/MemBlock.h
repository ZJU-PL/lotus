//
// Created by peiming on 12/18/19.
//

#ifndef ASER_PTA_MEMBLOCK_H
#define ASER_PTA_MEMBLOCK_H

#include <llvm/ADT/IndexedMap.h>

#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/AllocSite.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/Layout/MemLayout.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSObject.h"

namespace aser {

// forward declarations
template <typename ctx> class FSMemModel;
template <typename ctx> class FIMemBlock;
template <typename ctx> class ScalarMemBlock;
template <typename ctx> class AggregateMemBlock;

bool isArrayExistAtOffset(const std::map<size_t, ArrayLayout *> &arrayMap, size_t pOffset, size_t elementSize);

enum class MemBlockKind {
    // Array, Structure
    Aggregate = 0,
    // field-insensitive blocks (maybe a heap allocation site but we can not determine the type)
    FIBlock = 1,
    // primitive type (can not be indexed)
    // e.g., float, integer or pointer
    Scalar = 2,
};

// each memory block corresponding to one allocation site
template <typename ctx>
class MemBlock {
private:
    const MemBlockKind kind;
    const AllocSite<ctx> allocSite;

protected:
    MemBlock(const ctx *c, const llvm::Value *v, const AllocType t, const MemBlockKind kind)
        : allocSite(c, v, t), kind(kind) {};

public:
    [[nodiscard]] inline const ctx *getContext() const {
        return allocSite.getContext();
    }

    [[nodiscard]] inline const llvm::Value *getValue()  const {
        return allocSite.getValue();
    }

    [[nodiscard]] inline const AllocSite<ctx> &getAllocSite() const {
        return allocSite;
    }

    [[nodiscard]] inline bool validateStepSize(size_t pOffset, size_t stepSize) {
        switch (kind) {
            case MemBlockKind::Aggregate:
                return static_cast<AggregateMemBlock<ctx> *>(this)->validateStepSize(pOffset, stepSize);
            case MemBlockKind::FIBlock:
                return true;
            case MemBlockKind::Scalar : {
                // can not index Scalar Object.
                return false;
            }
        }
    }

    [[nodiscard]] inline bool isFIBlock() {
        return kind == MemBlockKind::FIBlock;
    }

    // nullptr if the memory block can not be indexed (scalar memory object) and the offset is non-zero
    // else the corresponding object
    [[nodiscard]] inline const FSObject<ctx> *getObjectAt(size_t offset) {
        switch (kind) {
            case MemBlockKind::Aggregate:
                return static_cast<AggregateMemBlock<ctx> *>(this)->indexMemoryBlock(offset);
            case MemBlockKind::FIBlock:
                return &static_cast<FIMemBlock<ctx> *>(this)->object;
            case MemBlockKind::Scalar : {
                if (offset == 0) {
                    return &static_cast<ScalarMemBlock<ctx> *>(this)->object;
                }
                return nullptr;
            }
        }
    }

    [[nodiscard]] inline const FSObject<ctx> *getPtrObjectAt(size_t offset) {
        switch (kind) {
            case MemBlockKind::Aggregate:
                return static_cast<AggregateMemBlock<ctx> *>(this)->indexPtrInMemoryBlock(offset);
            case MemBlockKind::FIBlock:
                return &static_cast<FIMemBlock<ctx> *>(this)->object;
            case MemBlockKind::Scalar : {
                if (offset == 0) {
                    return &static_cast<ScalarMemBlock<ctx> *>(this)->object;
                }
                return nullptr;
            }
        }
    }
};

// memory block with unknown type
template <typename ctx>
class FIMemBlock : public MemBlock<ctx> {
private:
    FSObject<ctx> object;
public:
    FIMemBlock(const ctx *c, const llvm::Value *v, const AllocType t)
        : MemBlock<ctx>(c, v, t, MemBlockKind::FIBlock), object(this, 0, 0) {}

    friend MemBlock<ctx>;
};

// memory block that stores primitive type (single pointer or a single integer..)
template <typename ctx>
class ScalarMemBlock : public MemBlock<ctx> {
private:
    FSObject<ctx> object;
public:
    ScalarMemBlock(const ctx *c, const llvm::Value *v, const AllocType t)
        : MemBlock<ctx>(c, v, t, MemBlockKind::Scalar), object(this, 0, 0) {}

    friend MemBlock<ctx>;
};

// array, structure
template <typename ctx>
class AggregateMemBlock : public MemBlock<ctx> {
private:
    // the allocation site of the memory block
    const MemLayout *layout;

//    // TODO: change it to vector
//    // map from layout offset to a object,
//    std::map<size_t, FSObject<ctx>> objectMap;

    // this vector is indexed by logical indices
    std::vector<std::unique_ptr<FSObject<ctx>>> fieldObjs;
    // offset is the physical offset
    FSObject<ctx> *indexMemoryBlock(size_t pOffset) {
        if (pOffset == 0) {
            // fast path
            return fieldObjs[0].get();
        }

        // 1st, convert physical offset to layout offset.
        size_t lOffset = layout->indexPhysicalOffset(pOffset);
        int fieldNum = layout->getLogicalOffset(lOffset);
        if (fieldNum >= 0) {
            // TODO: this is the bottle neck
            // 2nd, index the memory block, return cached object or create a new object.
            if (fieldObjs[fieldNum].get() == nullptr) {
                fieldObjs[fieldNum] = std::unique_ptr<FSObject<ctx>>(new FSObject<ctx>(this, pOffset, lOffset));
            }
            return fieldObjs[fieldNum].get();

//            return &(objectMap.emplace(std::piecewise_construct,
//                                       std::forward_as_tuple(lOffset),
//                                       std::forward_as_tuple(this, pOffset, lOffset)).first->second);
            //return &(objectMap.emplace(lOffset, this, pOffset, lOffset).first->second);
        }

        // the computed layout offset can not be indexed
        return nullptr;
    }

    FSObject<ctx> *indexPtrInMemoryBlock(size_t pOffset) {
        if (pOffset == 0) {
            // fast path
            // return &(objectMap.find(0)->second);
            return fieldObjs[0].get();
        }

        // 1st, convert physical offset to layout offset.
        size_t lOffset = layout->indexPhysicalOffset(pOffset);
        int fieldNum = layout->getLogicalOffset(lOffset);
        if (fieldNum >= 0  &&
            layout->offsetIsPtr(lOffset)) {
            // 2nd, index the memory block, return cached object or create a new object.
            if (fieldObjs[fieldNum].get() == nullptr) {
                fieldObjs[fieldNum] = std::unique_ptr<FSObject<ctx>>(new FSObject<ctx>(this, pOffset, lOffset));
            }
            return fieldObjs[fieldNum].get();

//            if (layout->offsetIndexable(lOffset) &&
//            // 2nd, index the memory block, return cached object or create a new object.
//            return &(objectMap.emplace(std::piecewise_construct,
//                                       std::forward_as_tuple(lOffset),
//                                       std::forward_as_tuple(this, pOffset, lOffset)).first->second);
        }

        // the computed layout offset can not be indexed
        return nullptr;
    }

    [[nodiscard]] inline bool validateStepSize(size_t pOffset, size_t stepSize) const {
        return isArrayExistAtOffset(layout->getSubArrayMap(), pOffset, stepSize);
    }

public:
    AggregateMemBlock(const ctx *c, const llvm::Value *v, const AllocType t, const MemLayout *layout)
        : MemBlock<ctx>(c, v, t, MemBlockKind::Aggregate), layout(layout) {
        // insert the first objects, other object are lazily initialized
        // objectMap.try_emplace(0 /*key*/, this, 0, 0);
        if (layout->getNumIndexableElem() == 0) {
            fieldObjs.resize(1); // model zero-sized object as if it has at least one element
        } else {
            fieldObjs.resize(layout->getNumIndexableElem());
        }

        fieldObjs[0].reset(new FSObject<ctx>(this, 0, 0));
//        objectMap.emplace(std::piecewise_construct,
//                          std::forward_as_tuple(0),
//                          std::forward_as_tuple(this, 0, 0));
    }

    friend MemBlock<ctx>;
};


}  // namespace aser

#endif  // ASER_PTA_MEMBLOCK_H
