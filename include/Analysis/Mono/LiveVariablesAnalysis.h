
#ifndef ANALYSIS_LIVEVARIABLESANALYSIS_H_
#define ANALYSIS_LIVEVARIABLESANALYSIS_H_

#include "Analysis/Mono/DataFlowResult.h"

namespace llvm {
class Function;
}

using namespace llvm;

/**
 * @brief Run SSA register liveness analysis on a function
 * 
 * This is a backward dataflow analysis that computes which SSA values (registers)
 * are "live" (will be used in the future) at each program point.
 * 
 * Dataflow equations for SSA form:
 *   - GEN[n] = { v | v is an SSA value used by n }
 *   - KILL[n] = { n } if n produces a non-void SSA value, ∅ otherwise
 *   - IN[n]  = (OUT[n] - KILL[n]) ∪ GEN[n]
 *   - OUT[n] = ⋃ IN[s] for all CFG successors s of n
 * 
 * Note: This analysis computes liveness of SSA registers only. It does NOT track
 * memory location liveness. Memory liveness requires a separate analysis operating
 * on MemorySSA def-use chains or explicit memory location lattices.
 * 
 * @param f The function to analyze
 * @return DataFlowResult containing live SSA value sets for each instruction
 */
DataFlowResult *runLiveVariablesAnalysis(Function *f);

#endif // ANALYSIS_LIVEVARIABLESANALYSIS_H_

