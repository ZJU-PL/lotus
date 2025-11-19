#ifndef LOCK_MISMATCH_CHECKER_H
#define LOCK_MISMATCH_CHECKER_H

#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Analysis/Concurrency/LockSetAnalysis.h"
#include "Analysis/Concurrency/ThreadAPI.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>

#include <vector>
#include <string>

namespace concurrency {

/**
 * @brief Specialized checker for lock usage errors
 * 
 * Detects:
 * - Double lock (for non-recursive locks)
 * - Double unlock
 * - Unlock without lock
 * - Lock leaks (returning with lock held)
 */
class LockMismatchChecker {
public:
    explicit LockMismatchChecker(llvm::Module& module,
                                mhp::LockSetAnalysis* locksetAnalysis,
                                ThreadAPI* threadAPI);

    /**
     * @brief Check for lock misuse
     * @return Vector of bug reports
     */
    std::vector<ConcurrencyBugReport> checkLockMisuse();

private:
    llvm::Module& m_module;
    mhp::LockSetAnalysis* m_locksetAnalysis;
    ThreadAPI* m_threadAPI;

    // Helper methods
    std::string getInstructionLocation(const llvm::Instruction* inst) const;
};

} // namespace concurrency

#endif // LOCK_MISMATCH_CHECKER_H

