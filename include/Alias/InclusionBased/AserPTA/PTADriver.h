//
// PTADriver.h
//
// Driver code for running pointer analysis passes, including PTADriverPass
// and runAnalysis function.
//

#pragma once

#include "Alias/InclusionBased/AserPTA/AnalysisOptions.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveASMInstPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/StandardHeapAPIRewritePass.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace aser {

/// @brief LLVM module pass that drives pointer analysis execution.
/// @tparam PTASolver The pointer analysis solver type to use.
template <typename PTASolver> class PTADriverPass : public llvm::ModulePass {
private:
  bool dumpStats;
  AnalysisOptions options;

public:
  static char ID;
  PTADriverPass(bool dumpStats = true,
                const AnalysisOptions &options = AnalysisOptions())
      : ModulePass(ID), dumpStats(dumpStats), options(options) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<PointerAnalysisPass<PTASolver>>();
  }

  bool runOnModule(llvm::Module &M) override {
    if (dumpStats) {
      llvm::ResetStatistics();
    }

    getAnalysis<PointerAnalysisPass<PTASolver>>().analyze(&M, "main",
                                                          options);

    if (dumpStats) {
      llvm::PrintStatistics(llvm::outs());
      llvm::ResetStatistics();
    }

    getAnalysis<PointerAnalysisPass<PTASolver>>().release();
    return false;
  }
};

template <typename PTASolver> char PTADriverPass<PTASolver>::ID = 0;

/// @brief Runs pointer analysis on a module with preprocessing passes.
/// @tparam Solver The pointer analysis solver type to use.
/// @param M The LLVM module to analyze.
/// @param dumpStats Whether to dump analysis statistics.
template <typename Solver>
void runAnalysis(llvm::Module &M, bool dumpStats,
                 const AnalysisOptions &options) {
  llvm::legacy::PassManager passes;

  // Preprocessing passes
  llvm::errs() << "Preprocessing IR...\n";
  passes.add(new CanonicalizeGEPPass());
  passes.add(new LoweringMemCpyPass());
  passes.add(new RemoveExceptionHandlerPass());
  passes.add(new RemoveASMInstPass());
  passes.add(new StandardHeapAPIRewritePass());

  // Analysis passes
  auto *ptaPass = new PointerAnalysisPass<Solver>(options);
  passes.add(ptaPass);
  passes.add(new PTADriverPass<Solver>(dumpStats, options));

  llvm::errs() << "Running pointer analysis...\n";
  passes.run(M);
  llvm::errs() << "Analysis completed.\n";
}

/// Backward-compatible solve-only entry point.
template <typename Solver>
void runAnalysis(llvm::Module &M, bool dumpStats = true) {
  runAnalysis<Solver>(M, dumpStats, AnalysisOptions());
}

/// Performs pointer analysis with optional points-to cache I/O.
template <typename Solver>
void runAnalysis(llvm::Module &M, const AnalysisOptions &options,
                 bool dumpStats = true) {
  runAnalysis<Solver>(M, dumpStats, options);
}

} // namespace aser

