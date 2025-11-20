#ifndef NPA_BIT_VECTOR_SOLVER_H
#define NPA_BIT_VECTOR_SOLVER_H

#include "Dataflow/NPA/NPA.h"
#include "Dataflow/NPA/BitVectorDomain.h"
#include "Dataflow/NPA/BitVectorInfo.h"
#include <unordered_map>

namespace npa {

enum class SolverStrategy {
    Kleene,
    Newton
};

/**
 * @brief Generic solver for bit-vector dataflow problems using NPA
 */
class BitVectorSolver {
public:
    using ResultMap = std::unordered_map<const llvm::BasicBlock*, llvm::APInt>;
    
    struct Result {
        ResultMap IN;
        ResultMap OUT;
        Stat stats;
    };

    /**
     * @brief Run the analysis on a function
     * @param F The target function
     * @param info The problem definition
     * @param strategy The solver strategy (Kleene or Newton)
     * @param verbose Enable verbose logging
     * @return The dataflow result (IN/OUT sets for each block)
     */
    static Result run(llvm::Function &F, 
                      const BitVectorInfo &info, 
                      SolverStrategy strategy = SolverStrategy::Newton,
                      bool verbose = false);
};

} // namespace npa

#endif // NPA_BIT_VECTOR_SOLVER_H
