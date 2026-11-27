#pragma once

#include "Alias/InclusionBased/TPA/Util/Iterator/IteratorRange.h"

#include <llvm/ADT/DenseSet.h>

namespace llvm {
class Value;
} // namespace llvm

namespace tpa {
class MemoryObject;
} // namespace tpa

namespace tpa::taint {

class ModRefFunctionSummary {
private:
  using ValueSet = llvm::DenseSet<const llvm::Value *>;
  ValueSet valueReadSet;

  using MemSet = llvm::DenseSet<const tpa::MemoryObject *>;
  MemSet memReadSet, memWriteSet;

public:
  using const_value_iterator = ValueSet::const_iterator;
  using const_mem_iterator = MemSet::const_iterator;

  bool addValueRead(const llvm::Value *val) {
    return valueReadSet.insert(val).second;
  }

  bool addMemoryRead(const tpa::MemoryObject *loc) {
    return memReadSet.insert(loc).second;
  }

  template <typename Iterator>
  bool addMemoryRead(Iterator begin, Iterator end) {
    bool ret = false;
    for (auto itr = begin; itr != end; ++itr)
      ret |= addMemoryRead(*itr);
    return ret;
  }

  bool addMemoryWrite(const tpa::MemoryObject *loc) {
    return memWriteSet.insert(loc).second;
  }

  template <typename Iterator>
  bool addMemoryWrite(Iterator begin, Iterator end) {
    bool ret = false;
    for (auto itr = begin; itr != end; ++itr)
      ret |= addMemoryWrite(*itr);
    return ret;
  }

  bool isValueRead(const llvm::Value *val) const {
    return valueReadSet.count(val);
  }

  bool isMemoryRead(const tpa::MemoryObject *loc) const {
    return memReadSet.count(loc);
  }

  bool isMemoryWritten(const tpa::MemoryObject *loc) const {
    return memWriteSet.count(loc);
  }

  auto value_reads() const {
    return util::iteratorRange(valueReadSet.begin(), valueReadSet.end());
  }
  auto mem_reads() const {
    return util::iteratorRange(memReadSet.begin(), memReadSet.end());
  }
  auto mem_writes() const {
    return util::iteratorRange(memWriteSet.begin(), memWriteSet.end());
  }

  bool empty() const {
    return valueReadSet.empty() && memReadSet.empty() && memWriteSet.empty();
  }

  void clear() {
    valueReadSet.clear();
    memReadSet.clear();
    memWriteSet.clear();
  }
  void reserveValueReads(size_t n) { valueReadSet.reserve(n); }
  void reserveMemoryReads(size_t n) { memReadSet.reserve(n); }
  void reserveMemoryWrites(size_t n) { memWriteSet.reserve(n); }
};

} // namespace tpa::taint
