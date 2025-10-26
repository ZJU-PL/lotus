#pragma once
#include "IR/PDG/Graph.h"
#include "IR/PDG/PDGAliasWrapper.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include <memory>

namespace pdg
{
  class DataDependencyGraph : public llvm::ModulePass
  {
  public:
    static char ID;
    DataDependencyGraph() : llvm::ModulePass(ID) {};
    void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
    llvm::StringRef getPassName() const override { return "Data Dependency Graph"; }
    bool runOnModule(llvm::Module &M) override;
    void addDefUseEdges(llvm::Instruction &inst);
    void addRAWEdges(llvm::Instruction &inst);
    void addAliasEdges(llvm::Instruction &inst);
    llvm::AliasResult queryAliasUnderApproximate(llvm::Value &v1, llvm::Value &v2);
    llvm::AliasResult queryAliasOverApproximate(llvm::Value &v1, llvm::Value &v2);

  private:
    llvm::MemoryDependenceResults *_mem_dep_res;
    std::unique_ptr<PDGAliasWrapper> _alias_wrapper_over;  // For over-approximation (Andersen)
    std::unique_ptr<PDGAliasWrapper> _alias_wrapper_under; // For under-approximation (syntactic)
  };
} // namespace pdg
