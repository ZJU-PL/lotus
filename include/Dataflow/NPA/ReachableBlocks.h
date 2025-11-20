#ifndef NPA_REACHABLE_BLOCKS_H
#define NPA_REACHABLE_BLOCKS_H

#include "Dataflow/NPA/BitVectorSolver.h"
#include <llvm/IR/Function.h>
#include <set>

namespace npa {

/**
 * @brief Simple Reachable Blocks analysis using NPA BitVector framework.
 * Determines which basic blocks are reachable from the entry point.
 * 
 * Note: This is a trivial analysis (all reachable blocks will have bit 1)
 * but demonstrates handling of single-bit boolean dataflow.
 */
class ReachableBlocks {
public:
    static std::set<const llvm::BasicBlock*> run(llvm::Function &F, 
                                                 SolverStrategy strategy = SolverStrategy::Newton);
};

} // namespace npa

#endif // NPA_REACHABLE_BLOCKS_H

