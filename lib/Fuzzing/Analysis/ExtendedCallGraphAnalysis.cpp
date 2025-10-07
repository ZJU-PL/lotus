#include "Fuzzing/Analysis/ExtendedCallGraph.hpp"

#include <llvm/Analysis/CallGraph.h>

using namespace llvm;

AnalysisKey ExtendedCallGraphAnalysis::Key;

ExtendedCallGraphAnalysis::Result
ExtendedCallGraphAnalysis::run(Module &M, ModuleAnalysisManager &MAM) {
  // Return a basic LLVM call graph without SVF-based indirect call resolution
  return CallGraph(M);
}