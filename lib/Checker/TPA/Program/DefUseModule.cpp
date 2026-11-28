#include "Checker/TPA/Program/DefUseModule.h"

#include "Alias/InclusionBased/TPA/PointerAnalysis/MemoryModel/MemoryObject.h"
#include "Checker/TPA/Support/ExternalModels.h"

#include <algorithm>
#include <cstddef>
#include <limits>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace tpa::taint {

void MemoryGroupTable::buildChunkMasks() {
  groupChunkMasks.clear();
  groupChunkMasks.resize(groupLocs.size());
  llvm::SmallDenseMap<uint32_t, uint64_t, 16> chunkMap;
  for (size_t g = 0; g < groupLocs.size(); ++g) {
    chunkMap.clear();
    for (const auto *loc : groupLocs[g]) {
      const uint32_t id = loc->getId();
      chunkMap[id >> 5] |= (3ULL << ((id & 31u) << 1));
    }
    auto &out = groupChunkMasks[g];
    out.reserve(chunkMap.size());
    for (const auto &entry : chunkMap)
      out.push_back(ChunkMask{entry.first, entry.second});
    std::sort(out.begin(), out.end(),
              [](const ChunkMask &a, const ChunkMask &b) {
                return a.chunkIdx < b.chunkIdx;
              });
  }
}

DefUseInstruction::DefUseInstruction(const Instruction &i) : inst(&i), rpo(0) {}

DefUseInstruction::DefUseInstruction(const Function *f)
    : inst(f), rpo(std::numeric_limits<size_t>::max()) {}

const Instruction *DefUseInstruction::getInstruction() const {
  return cast<Instruction>(inst);
}

const Function *DefUseInstruction::getFunction() const {
  if (const auto *f = dyn_cast<Function>(inst))
    return f;
  else {
    const auto *i = cast<Instruction>(inst);
    return i->getParent()->getParent();
  }
}

bool DefUseInstruction::isEntryInstruction() const {
  return isa<Function>(inst);
}

bool DefUseInstruction::isCallInstruction() const {
  return isa<CallBase>(inst);
}

bool DefUseInstruction::isReturnInstruction() const {
  return isa<ReturnInst>(inst);
}

DefUseInstruction *
DefUseFunction::getDefUseInstruction(const Instruction *inst) {
  auto itr = instMap.find(inst);
  if (itr != instMap.end())
    return itr->second;
  else
    return nullptr;
}
const DefUseInstruction *
DefUseFunction::getDefUseInstruction(const Instruction *inst) const {
  auto itr = instMap.find(inst);
  if (itr != instMap.end())
    return itr->second;
  else
    return nullptr;
}

void DefUseFunction::finalizeMemSuccEdges() {
  entryInst.finalizeMemSuccEdges();
  for (auto &node : instStorage)
    node.finalizeMemSuccEdges();
}

void DefUseFunction::ensureMemPreds() const {
  if (memPredsBuilt)
    return;
  auto addFrom = [](const DefUseInstruction &src) {
    auto *mutableSrc = const_cast<DefUseInstruction *>(&src);
    for (const auto &entry : src.mem_succs())
      for (auto *dst : entry.second)
        dst->appendMemPredEdge(entry.first, mutableSrc);
    for (const auto &entry : src.group_mem_succs())
      for (auto *dst : entry.second)
        dst->appendGroupMemPredEdge(entry.first, mutableSrc);
  };
  addFrom(entryInst);
  for (const auto &node : instStorage)
    addFrom(node);
  entryInst.finalizeMemPredEdges();
  for (const auto &node : instStorage)
    node.finalizeMemPredEdges();
  memPredsBuilt = true;
}

DefUseFunction::DefUseFunction(const Function &f) : function(f), entryInst(&f) {
  entryInst.setMemoryGroups(&memGroups);
  for (const auto *bb : llvm::depth_first(&f.getEntryBlock())) {
    for (auto const &inst : *bb) {
      if (isDebugIntrinsic(inst))
        continue;
      if (const auto *brInst = dyn_cast<BranchInst>(&inst))
        if (brInst->isUnconditional())
          continue;

      instStorage.emplace_back(inst);
      DefUseInstruction *duInst = &instStorage.back();
      duInst->setMemoryGroups(&memGroups);
      instMap.try_emplace(&inst, duInst);

      if (isa<ReturnInst>(&inst)) {
        exitInsts.push_back(duInst);
      }
      if (isa<ReturnInst>(inst) || isa<ResumeInst>(inst))
        memoryExitInsts.push_back(duInst);
    }
  }
}

DefUseModule::DefUseModule(
    const Module &m, const std::unordered_set<const Function *> &reachable)
    : module(m), entryFunc(nullptr) {
  for (auto const &f : module) {
    if (f.isDeclaration() || !reachable.count(&f))
      continue;

    auto itr = funMap
                   .emplace(std::piecewise_construct, std::forward_as_tuple(&f),
                            std::forward_as_tuple(f))
                   .first;

    if (f.getName() == "main") {
      assert(entryFunc == nullptr);
      entryFunc = &itr->second;
    }
  }
}

const DefUseFunction &DefUseModule::getDefUseFunction(const Function *f) const {
  auto itr = funMap.find(f);
  assert(itr != funMap.end());
  return itr->second;
}

} // namespace tpa::taint
