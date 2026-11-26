#pragma once

#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"
#include "Alias/InclusionBased/TPA/Util/Iterator/IteratorRange.h"

#include <cassert>
#include <cstddef>
#include <unordered_map>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace tpa {
class MemoryObject;
} // namespace tpa

namespace tpa::taint {

class DefUseInstruction {
private:
  // If entry inst, this field stores the function it belongs to
  // Otherwise, this field stores the corresponding llvm instruction
  const llvm::Value *inst;
  // The reverse postorder number of this node
  size_t rpo;

  using NodeSet = util::VectorSet<DefUseInstruction *>;
  NodeSet topSucc, topPred;

  using NodeMap = std::unordered_map<const tpa::MemoryObject *, NodeSet>;
  NodeMap memSucc, memPred;

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
  auto mem_succs(const tpa::MemoryObject *obj) {
    auto itr = memSucc.find(obj);
    if (itr == memSucc.end())
      return util::iteratorRange(iterator(), iterator());
    else
      return util::iteratorRange(itr->second.begin(), itr->second.end());
  }
  auto mem_succs(const tpa::MemoryObject *obj) const {
    auto itr = memSucc.find(obj);
    if (itr == memSucc.end())
      return util::iteratorRange(const_iterator(), const_iterator());
    else
      return util::iteratorRange(itr->second.begin(), itr->second.end());
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
