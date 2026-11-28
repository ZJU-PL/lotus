// Implementation of the SemiSparsePointerAnalysis class.
//
// This file implements the high-level driver for the semi-sparse pointer
// analysis. It orchestrates the two main phases of the analysis:
// 1. Global Initialization: Processing global variables and their initializers.
// 2. Data-Flow Analysis: Running the fixpoint algorithm on the semi-sparse CFG.

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"

#include "Alias/InclusionBased/TPA/Context/Context.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/GlobalPointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/Initializer.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/ParallelSolver.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/SemiSparsePropagator.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Program/SemiSparseProgram.h"
#include "Alias/InclusionBased/TPA/Util/Log.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include <llvm/ADT/ScopeExit.h>

namespace tpa {

// Main entry point for the analysis on a semi-sparse program.
//
// The analysis proceeds in two distinct phases:
//
// Phase 1: Global Initialization
// Uses GlobalPointerAnalysis to scan the module for global variables and
// functions. It populates the initial Environment (Env) with mappings for
// globals and the initial Store with the effects of global initializers.
//
// Phase 2: Data-Flow Analysis
// Sets up GlobalState and the worklist engine. The engine uses:
// - GlobalState: Shared state (program, type maps, managers)
// - Memo: Memoization table for avoiding redundant work
// - TransferFunction: Evaluator for individual program points
// - SemiSparsePropagator: Strategy for traversing the CFG and updating state
//
// The analysis runs until a fixpoint is reached.
void SemiSparsePointerAnalysis::runOnProgram(const SemiSparseProgram &ssProg) {
  runOnProgram(ssProg, Config{});
}

void SemiSparsePointerAnalysis::runOnProgram(const SemiSparseProgram &ssProg,
                                             Config config) {
  if (program)
    throw std::logic_error(
        "TPA analysis instances are single-run; construct a fresh analysis");
  if (config.parallel && !config.lookahead)
    throw std::invalid_argument("TPA parallel lookahead must be positive");
  program = &ssProg;
  stats = {};
  if (!config.parallel) {
    ptrManager.setThreadSafe(false);
    ptrManager.disablePublishedTracking();
    memManager.setThreadSafe(false);
  }
  auto restoreLocks = llvm::make_scope_exit([&] {
    ptrManager.setThreadSafe(true);
    memManager.setThreadSafe(true);
  });
  const auto started = std::chrono::steady_clock::now();
  LOG_INFO("Phase 1: Analyzing global pointers and initializers...");
  auto initStore = Store();
  // Initialize globals and get the starting environment and store.
  std::tie(env, initStore) =
      GlobalPointerAnalysis(ptrManager, memManager, ssProg.getTypeMap())
          .runOnModule(ssProg.getModule());
  LOG_INFO("Global pointer analysis completed");

  // Register pointer args of every function (covers callbacks only entered by
  // external code) so queries on them are well-defined.
  for (const auto &func : ssProg.getModule()) {
    for (const auto &arg : func.args()) {
      if (arg.getType()->isPointerTy())
        ptrManager.getOrCreatePointer(context::Context::getGlobalContext(),
                                      &arg);
    }
  }

  LOG_INFO("Phase 2: Running data-flow analysis...");
  // Construct the global state that will be passed to transfer functions.
  // Note that 'env' is passed by reference and will be updated during analysis.
  auto globalState = GlobalState(ptrManager, memManager, ssProg, extTable, env);

  // Seed the reference worklist before selecting the execution engine.
  auto worklist =
      Initializer(globalState, memo).runOnInitState(std::move(initStore));
  const auto solving = std::chrono::steady_clock::now();
  stats.initializationSeconds =
      std::chrono::duration<double>(solving - started).count();
  if (config.parallel) {
    solveParallel(globalState, memo, std::move(worklist), config, stats);
  } else {
    while (!worklist.empty()) {
      auto point = worklist.dequeue();
      auto result =
          TransferFunction(globalState, memo.lookup(point)).eval(point);
      SemiSparsePropagator(memo, worklist).propagate(result);
      ++stats.transfers;
      ++stats.evaluations;
    }
  }
  stats.solveSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - solving)
          .count();
  LOG_INFO("Data-flow analysis completed");
}

namespace {
bool sameObject(const MemoryObject *a, const MemoryObject *b) {
  return a->getAllocSite() == b->getAllocSite() &&
         a->getOffset() == b->getOffset() &&
         a->isSummaryObject() == b->isSummaryObject();
}
bool sameSet(PtsSet a, PtsSet b) {
  if (a.size() != b.size())
    return false;
  for (const MemoryObject *object : a)
    if (std::none_of(b.begin(), b.end(), [&](const MemoryObject *other) {
          return sameObject(object, other);
        }))
      return false;
  return true;
}
bool sameStore(const Store &a, const Store &b) {
  if (a.size() != b.size())
    return false;
  for (const auto &binding : a) {
    const auto found = std::find_if(b.begin(), b.end(), [&](const auto &other) {
      return sameObject(binding.first, other.first);
    });
    if (found == b.end() || !sameSet(binding.second, found->second))
      return false;
  }
  return true;
}
} // namespace

bool SemiSparsePointerAnalysis::hasSameSolution(
    const SemiSparsePointerAnalysis &other, std::string *difference) const {
  auto mismatch = [&](const char *reason) {
    if (difference)
      *difference = reason;
    return false;
  };
  if (difference)
    difference->clear();
  if (!program || program != other.program)
    return mismatch("analyses use different semi-sparse programs");
  if (env.size() != other.env.size())
    return mismatch("environment entry count differs");
  for (const auto &binding : env) {
    const Pointer *pointer = other.ptrManager.getPointer(
        binding.first->getContext(), binding.first->getValue());
    if (!pointer || !other.env.hasBinding(pointer) ||
        !sameSet(binding.second, other.env.lookup(pointer)))
      return mismatch("environment points-to binding differs");
  }
  if (memo.entries().size() != other.memo.entries().size())
    return mismatch("program-point memory entry count differs");
  for (const auto &entry : memo.entries()) {
    const auto found = other.memo.lookup(entry.first);
    if (!found || !sameStore(*entry.second, *found))
      return mismatch("program-point incoming store differs");
  }
  return true;
}

// Internal implementation of getPtsSet required by the CRTP base class.
// Looks up the points-to set in the computed environment.
PtsSet SemiSparsePointerAnalysis::getPtsSetImpl(const Pointer *ptr) const {
  return env.lookup(ptr);
}

} // namespace tpa
