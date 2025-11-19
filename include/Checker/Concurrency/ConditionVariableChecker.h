#ifndef CONDITION_VARIABLE_CHECKER_H
#define CONDITION_VARIABLE_CHECKER_H

#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Analysis/Concurrency/ThreadAPI.h"
#include "Analysis/Concurrency/LockSetAnalysis.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>

#include <vector>
#include <string>

namespace concurrency {

/**
 * @brief Specialized checker for condition variable misuse
 */
class ConditionVariableChecker {
public:
    explicit ConditionVariableChecker(llvm::Module& module,
                                     ThreadAPI* threadAPI,
                                     mhp::LockSetAnalysis* locksetAnalysis);

    /**
     * @brief Check for condition variable misuse
     * @return Vector of bug reports
     */
    std::vector<ConcurrencyBugReport> checkConditionVariables();

private:
    llvm::Module& m_module;
    ThreadAPI* m_threadAPI;
    mhp::LockSetAnalysis* m_locksetAnalysis;

    // Helper methods
    std::string getInstructionLocation(const llvm::Instruction* inst) const;
    const llvm::Value* getMutexForCV(const llvm::Instruction* waitInst) const;
};

} // namespace concurrency

#endif // CONDITION_VARIABLE_CHECKER_H

