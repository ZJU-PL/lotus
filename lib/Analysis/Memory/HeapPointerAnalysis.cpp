#include "Analysis/Memory/HeapPointerAnalysis.h"

#include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
#include "Utils/LLVM/CallUtils.h"

#include <vector>

#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace lotus {

void HeapPointerAnalysis::analyze(Module &module, DyckAliasAnalysis &alias,
                                  ArrayRef<std::string> allocators,
                                  ArrayRef<std::string> releases) {
  Alias = &alias;
  MayHeap.clear();
  auto seed = [&](ArrayRef<std::string> names, bool release) {
    for (const auto &name : names) {
      auto *function = module.getFunction(name);
      if (!function)
        continue;
      for (auto *user : function->users()) {
        auto *call = dyn_cast<CallBase>(user);
        if (!call || (release && call->arg_empty()))
          continue;
        Value *value = release ? call->getArgOperand(0) : call;
        if (auto *node = alias.getDyckGraph()->findDyckVertex(value))
          MayHeap.insert(node);
      }
    }
  };
  seed(allocators, false);
  seed(releases, true);

  std::vector<DyckGraphNode *> pending(MayHeap.begin(), MayHeap.end());
  while (!pending.empty()) {
    auto *node = pending.back();
    pending.pop_back();
    auto visit = [&](std::set<void *> &labels, bool incoming) {
      for (auto *label : labels) {
        if (!static_cast<DyckGraphEdgeLabel *>(label)->isLabelTy(
                DyckGraphEdgeLabel::LT_Offset))
          continue;
        auto *neighbors =
            incoming ? node->getInVertices(label) : node->getOutVertices(label);
        if (!neighbors)
          continue;
        for (auto *neighbor : *neighbors)
          if (MayHeap.insert(neighbor).second)
            pending.push_back(neighbor);
      }
    };
    visit(node->getInLabels(), true);
    visit(node->getOutLabels(), false);
  }
}

bool HeapPointerAnalysis::mayHeapPtr(Value *value) const {
  assert(Alias && value);
  auto *node = Alias->getDyckGraph()->findDyckVertex(value);
  return !node || MayHeap.count(node);
}

} // namespace lotus
