#ifndef UNDERAPPROX_EQUIVDB_H
#define UNDERAPPROX_EQUIVDB_H

#include <llvm/IR/Function.h>
#include <unordered_map>
#include <vector>

namespace UnderApprox {

/// Union–find + congruence-closure over one function’s IR.
/// The database is built once, then queried O(α(N)).
class EquivDB {
public:
  EquivDB(llvm::Function &F);

  bool mustAlias(const llvm::Value *A, const llvm::Value *B) const;

private:
  // ---------- union–find ----------------------------------------------------
  using IdTy = unsigned;
  struct Node { IdTy Parent; uint8_t Rank; };

  IdTy id(const llvm::Value *);
  IdTy find(IdTy);
  void unite(IdTy, IdTy);

  std::vector<Node> Nodes;
  std::vector<const llvm::Value *> Id2Val;
  std::unordered_map<const llvm::Value *, IdTy> Val2Id;

  // ---------- saturation work-lists ----------------------------------------
  struct WatchInfo {
    llvm::SmallVector<llvm::Instruction *, 2> Users;
  };
  std::vector<WatchInfo> Watches; // indexed by UF root

  const llvm::DataLayout &DL;
  llvm::Function &F;

  void seedAtomicEqualities(std::vector<std::pair<const llvm::Value *,
                                                  const llvm::Value *>> &WL);
  void propagate(std::vector<std::pair<const llvm::Value *,
                                       const llvm::Value *>> &WL);
  void registerWatch(const llvm::Value *Op, llvm::Instruction *I);
  bool operandsInSameClass(const llvm::Instruction *I) const;
};

} // end namespace UnderApprox
#endif