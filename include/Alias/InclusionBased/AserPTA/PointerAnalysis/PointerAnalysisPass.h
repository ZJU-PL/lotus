//
// Created by peiming on 3/24/20.
// Updated for LLVM 14 compatibility
//

#pragma once

#include "Alias/InclusionBased/AserPTA/Util/Log.h"

#include <chrono>
#include <memory>

#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Pass.h>

/// @brief LLVM module pass that runs pointer analysis using a specified solver.
/// @tparam Solver The pointer analysis solver type to use.
template <typename Solver> class PointerAnalysisPass : public llvm::ModulePass {
private:
  std::unique_ptr<Solver> solver; // owner of the solver

public:
  static char ID;
  PointerAnalysisPass() : llvm::ModulePass(ID), solver(nullptr) {}

  /// @brief Runs pointer analysis on the given module.
  /// @param M The LLVM module to analyze.
  /// @return false (analysis pass does not modify IR).
  bool runOnModule(llvm::Module &M) override {
    analyze(&M);
    return false; // Analysis doesn't modify IR
  }

  /// @brief Analyzes the given module with the specified entry function.
  /// @param M Pointer to the LLVM module.
  /// @param entry Name of the entry function (default: "main").
  void analyze(llvm::Module *M, llvm::StringRef entry = "main") {
    if (solver.get() != nullptr) {
      if (solver->getLLVMModule() == M &&
          entry.equals(solver->getEntryName())) {
        return;
      }
    }
    // release previous context
    solver.reset(new Solver());

    LOG_INFO("PTA start to run");
    auto start = std::chrono::steady_clock::now();
    solver->analyze(M, entry);

    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;

    LOG_INFO(
        "PTA finished, running time : {} ms",
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_seconds)
            .count());
  }

  /// @brief Returns the pointer analysis solver instance.
  /// @return Pointer to the solver (must call analyze() first).
  Solver *getPTA() const {
    assert(solver.get() != nullptr &&
           "call analyze() before getting the pta instance");
    return solver.get();
  }

  /// @brief Releases the solver and frees associated memory.
  void release() {
    // release the memory hold by the correct solver
    solver.reset(nullptr);
  }
};

template <typename Solver> char PointerAnalysisPass<Solver>::ID = 0;

