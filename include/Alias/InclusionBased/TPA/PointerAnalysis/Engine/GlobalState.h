#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/StorePruner.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/CallGraph.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/Env.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/FunctionContext.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/ProgramPoint.h"

namespace annotation {
class ExternalPointerTable;
} // namespace annotation

namespace tpa {

class MemoryManager;
class PointerManager;
class SemiSparseProgram;

// Global state for the pointer analysis engine
//
// Holds all the state needed by the analysis, passed to transfer functions.
// This is essentially a handle to all the analysis data structures.
//
// Components:
// - PointerManager: Manages Pointer objects
// - MemoryManager: Manages MemoryObject objects
// - SemiSparseProgram: The program being analyzed
// - ExternalPointerTable: Annotations for library functions
// - Env: Points-to sets for top-level pointers
// - CallGraph: Call graph with context information
//
// Transfers mutate Env, CallGraph, and the canonical pointer/object registries.
// Parallel evaluations use private Env/CallGraph views and stage pointer
// visibility; only the coordinator publishes their effects.
class GlobalState {
private:
  PointerManager &ptrManager;
  MemoryManager &memManager;

  const SemiSparseProgram &prog;
  const annotation::ExternalPointerTable &extTable;

  Env &env;
  CallGraph<ProgramPoint, FunctionContext> callGraph;
  StorePrunerCache storePrunerCache;

public:
  // Constructor
  // Parameters:
  //   p - pointer manager
  //   m - memory manager
  //   s - semi-sparse program
  //   t - external pointer table
  //   e - initial environment
  GlobalState(PointerManager &p, MemoryManager &m, const SemiSparseProgram &s,
              const annotation::ExternalPointerTable &t, Env &e)
      : ptrManager(p), memManager(m), prog(s), extTable(t), env(e) {}

  // Access managers
  PointerManager &getPointerManager() { return ptrManager; }
  const PointerManager &getPointerManager() const { return ptrManager; }
  MemoryManager &getMemoryManager() { return memManager; }
  const MemoryManager &getMemoryManager() const { return memManager; }
  const SemiSparseProgram &getSemiSparseProgram() const { return prog; }
  const annotation::ExternalPointerTable &getExternalPointerTable() const {
    return extTable;
  }

  // Access environment (mutable during analysis)
  Env &getEnv() { return env; }
  const Env &getEnv() const { return env; }

  // Access call graph
  decltype(callGraph) &getCallGraph() { return callGraph; }
  const decltype(callGraph) &getCallGraph() const { return callGraph; }

  StorePrunerCache &getStorePrunerCache() { return storePrunerCache; }
};

} // namespace tpa
