#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

namespace llvm {
class Instruction;
class Function;
} // namespace llvm

namespace lotus {

class CallGraph {
public:
  CallGraph() = default;

  void addCallEdge(const llvm::Instruction *CS, const llvm::Function *Callee);

  [[nodiscard]] llvm::ArrayRef<const llvm::Function *>
  getCalleesOfCallAt(const llvm::Instruction *CS) const;

  [[nodiscard]] llvm::ArrayRef<const llvm::Instruction *>
  getCallersOf(const llvm::Function *F) const;

  [[nodiscard]] std::vector<const llvm::Function *>
  getAllVertexFunctions() const;

  [[nodiscard]] std::vector<const llvm::Instruction *>
  getAllVertexCallSites() const;

  [[nodiscard]] size_t getNumCallSites() const;
  [[nodiscard]] size_t getNumFunctions() const;
  [[nodiscard]] bool empty() const;

  void print(llvm::raw_ostream &OS) const;
  void printAsDot(llvm::raw_ostream &OS) const;
  void printAsJson(llvm::raw_ostream &OS) const;

private:
  llvm::DenseMap<const llvm::Function *,
                 llvm::SmallVector<const llvm::Instruction *, 4>>
      CallersOf;
  llvm::DenseMap<const llvm::Instruction *,
                 llvm::SmallVector<const llvm::Function *, 4>>
      CalleesOf;
};

} // namespace lotus
