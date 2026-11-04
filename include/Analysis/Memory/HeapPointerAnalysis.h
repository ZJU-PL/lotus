#pragma once

#include <set>
#include <string>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/Module.h>

class DyckAliasAnalysis;
class DyckGraphNode;

namespace lotus {

/// Classifies heap-associated alias classes and closes them under offsets.
/// Function models are supplied by the client so existing analyses retain
/// their own library profiles.
class HeapPointerAnalysis {
  DyckAliasAnalysis *Alias = nullptr;
  std::set<DyckGraphNode *> MayHeap;

public:
  void analyze(llvm::Module &module, DyckAliasAnalysis &alias,
               llvm::ArrayRef<std::string> allocators,
               llvm::ArrayRef<std::string> releases);
  bool mayHeapPtr(llvm::Value *value) const;
};

} // namespace lotus
