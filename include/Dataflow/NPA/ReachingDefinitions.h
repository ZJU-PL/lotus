#ifndef NPA_REACHING_DEFINITIONS_H
#define NPA_REACHING_DEFINITIONS_H

#include "Dataflow/NPA/BitVectorSolver.h"
#include <llvm/IR/Function.h>

namespace npa {

/**
 * @brief Standard Reaching Definitions analysis using NPA BitVector framework.
 * Computes which definitions (Instructions/Arguments) may reach each basic block.
 * 
 * Note: For LLVM SSA, this is trivial (Defs dominate uses), but serves as a
 * validation of the framework.
 */
class ReachingDefinitions {
public:
    static BitVectorSolver::Result run(llvm::Function &F, 
                                       SolverStrategy strategy = SolverStrategy::Newton);
};

} // namespace npa

#endif // NPA_REACHING_DEFINITIONS_H
