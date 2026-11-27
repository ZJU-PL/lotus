#pragma once

#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"
#include "Alias/InclusionBased/TPA/Util/Iterator/IteratorRange.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <llvm/ADT/DenseMap.h>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace tpa {
class MemoryObject;
} // namespace tpa

namespace tpa::taint {

// Function-scoped equivalence table partitioning memory objects that share
// identical read/write/entry/exit signatures inside a DefUseFunction.
// Storing memory def-use edges once per groupId avoids duplicating identical
// successor vectors across thousands of equivalent global/heap objects.
struct MemoryGroupTable {
  llvm::DenseMap<const tpa::MemoryObject *, uint32_t> locToGroup;
  std::vector<std::vector<const tpa::MemoryObject *>> groupLocs;

  const uint32_t *findGroup(const tpa::MemoryObject *obj) const {
    auto itr = locToGroup.find(obj);
    return itr == locToGroup.end() ? nullptr : &itr->second;
  }
  const std::vector<const tpa::MemoryObject *> &
  getGroupObjects(uint32_t groupId) const {
    return groupLocs[groupId];
  }
  void clear() {
    locToGroup.clear();
    groupLocs.clear();
  }
};

class DefUseInstruction {
private:
  // If entry inst, this field stores the function it belongs to
  // Otherwise, this field stores the corresponding llvm instruction
  const llvm::Value *inst;
  // The reverse postorder number of this node
  size_t rpo;

  using NodeSet = util::VectorSet<DefUseInstruction *>;
  NodeSet topSucc, topPred;

  using NodeMap = llvm::DenseMap<const tpa::MemoryObject *, NodeSet>;
  using GroupNodeMap = llvm::DenseMap<uint32_t, NodeSet>;
  NodeMap memSucc;
  mutable NodeMap memPred;
  GroupNodeMap groupMemSucc;
  mutable GroupNodeMap groupMemPred;
  const MemoryGroupTable *memGroups = nullptr;
  uint32_t numGroupSuccObjs = 0;

  DefUseInstruction(const llvm::Function *f);

public:
  using iterator = NodeSet::iterator;
  using const_iterator = NodeSet::const_iterator;
  using mem_succ_iterator = NodeMap::iterator;
  using const_mem_succ_iterator = NodeMap::const_iterator;

  DefUseInstruction(const llvm::Instruction &i);
  const llvm::Instruction *getInstruction() const;

  const llvm::Function *getFunction() const;
  bool isEntryInstruction() const;
  bool isCallInstruction() const;
  bool isReturnInstruction() const;

  void setMemoryGroups(const MemoryGroupTable *groups) { memGroups = groups; }

  size_t getPriority() const {
    assert(rpo != 0);
    return rpo;
  }
  void setPriority(size_t o) {
    assert(o != 0 && "0 cannot be used as a priority num!");
    rpo = o;
  }

  void insertTopLevelEdge(DefUseInstruction *node) {
    assert(node != nullptr);
    topSucc.insert(node);
    node->topPred.insert(this);
  }
  void insertMemLevelEdge(const tpa::MemoryObject *loc,
                          DefUseInstruction *node) {
    assert(loc != nullptr && node != nullptr);
    memSucc[loc].insert(node);
    node->memPred[loc].insert(this);
  }
  // Bulk forward-edge insertion used during graph construction; caller must
  // call finalizeMemSuccEdges() once construction finishes. Reverse memPred
  // edges are populated lazily on demand when precision-loss tracking runs.
  void appendMemSuccEdge(const tpa::MemoryObject *loc,
                         DefUseInstruction *node) {
    assert(loc != nullptr && node != nullptr);
    memSucc[loc].push_back_unsorted(node);
  }
  void appendGroupMemSuccEdge(uint32_t groupId, DefUseInstruction *node) {
    assert(node != nullptr);
    groupMemSucc[groupId].push_back_unsorted(node);
  }
  void finalizeMemSuccEdges() {
    for (auto &entry : memSucc)
      entry.second.finalize();
    numGroupSuccObjs = 0;
    for (auto &entry : groupMemSucc) {
      entry.second.finalize();
      if (memGroups && entry.first < memGroups->groupLocs.size())
        numGroupSuccObjs +=
            static_cast<uint32_t>(memGroups->groupLocs[entry.first].size());
    }
  }
  void appendMemPredEdge(const tpa::MemoryObject *loc,
                         DefUseInstruction *pred) const {
    assert(loc != nullptr && pred != nullptr);
    memPred[loc].push_back_unsorted(pred);
  }
  void appendGroupMemPredEdge(uint32_t groupId, DefUseInstruction *pred) const {
    assert(pred != nullptr);
    groupMemPred[groupId].push_back_unsorted(pred);
  }
  void finalizeMemPredEdges() const {
    for (auto &entry : memPred)
      entry.second.finalize();
    for (auto &entry : groupMemPred)
      entry.second.finalize();
  }

  auto top_succs() {
    return util::iteratorRange(topSucc.begin(), topSucc.end());
  }
  auto top_succs() const {
    return util::iteratorRange(topSucc.begin(), topSucc.end());
  }
  auto mem_succs() {
    return util::iteratorRange(memSucc.begin(), memSucc.end());
  }
  auto mem_succs() const {
    return util::iteratorRange(memSucc.begin(), memSucc.end());
  }
  auto group_mem_succs() const {
    return util::iteratorRange(groupMemSucc.begin(), groupMemSucc.end());
  }
  auto mem_succs(const tpa::MemoryObject *obj) {
    if (memGroups && !groupMemSucc.empty()) {
      if (const uint32_t *gid = memGroups->findGroup(obj)) {
        auto itr = groupMemSucc.find(*gid);
        if (itr != groupMemSucc.end())
          return util::iteratorRange(itr->second.begin(), itr->second.end());
      }
    }
    auto itr = memSucc.find(obj);
    if (itr == memSucc.end())
      return util::iteratorRange(iterator(), iterator());
    else
      return util::iteratorRange(itr->second.begin(), itr->second.end());
  }
  auto mem_succs(const tpa::MemoryObject *obj) const {
    if (const NodeSet *s = findMemSuccs(obj))
      return util::iteratorRange(s->begin(), s->end());
    return util::iteratorRange(const_iterator(), const_iterator());
  }
  const NodeSet *findMemSuccs(const tpa::MemoryObject *obj) const {
    if (memGroups && !groupMemSucc.empty()) {
      if (const uint32_t *gid = memGroups->findGroup(obj)) {
        auto itr = groupMemSucc.find(*gid);
        if (itr != groupMemSucc.end())
          return &itr->second;
      }
    }
    if (!memSucc.empty()) {
      auto itr = memSucc.find(obj);
      if (itr != memSucc.end())
        return &itr->second;
    }
    return nullptr;
  }
  const NodeSet *findMemPreds(const tpa::MemoryObject *obj) const {
    if (memGroups && !groupMemPred.empty()) {
      if (const uint32_t *gid = memGroups->findGroup(obj)) {
        auto itr = groupMemPred.find(*gid);
        if (itr != groupMemPred.end())
          return &itr->second;
      }
    }
    if (!memPred.empty()) {
      auto itr = memPred.find(obj);
      if (itr != memPred.end())
        return &itr->second;
    }
    return nullptr;
  }
  size_t numMemSuccObjects() const { return memSucc.size() + numGroupSuccObjs; }
  size_t countMemSuccEdges() const {
    size_t total = 0;
    for (const auto &entry : memSucc)
      total += entry.second.size();
    if (memGroups) {
      for (const auto &entry : groupMemSucc)
        total += memGroups->getGroupObjects(entry.first).size() *
                 entry.second.size();
    }
    return total;
  }
  template <typename Fn> void forEachMemSucc(Fn &&fn) const {
    for (const auto &entry : memSucc)
      fn(entry.first, entry.second);
    if (memGroups) {
      for (const auto &entry : groupMemSucc) {
        for (const auto *loc : memGroups->getGroupObjects(entry.first))
          fn(loc, entry.second);
      }
    }
  }
  template <typename Fn> void forEachMemPred(Fn &&fn) const {
    for (const auto &entry : memPred)
      fn(entry.first, entry.second);
    if (memGroups) {
      for (const auto &entry : groupMemPred) {
        for (const auto *loc : memGroups->getGroupObjects(entry.first))
          fn(loc, entry.second);
      }
    }
  }

  auto top_preds() {
    return util::iteratorRange(topPred.begin(), topPred.end());
  }
  auto top_preds() const {
    return util::iteratorRange(topPred.begin(), topPred.end());
  }
  auto mem_preds() {
    return util::iteratorRange(memPred.begin(), memPred.end());
  }
  auto mem_preds() const {
    return util::iteratorRange(memPred.begin(), memPred.end());
  }

  friend class DefUseFunction;
};

} // namespace tpa::taint
