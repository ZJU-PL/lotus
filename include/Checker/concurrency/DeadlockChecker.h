#ifndef DEADLOCK_CHECKER_H
#define DEADLOCK_CHECKER_H

#include "Checker/concurrency/ConcurrencyBugReport.h"
#include "Analysis/Concurrency/MHPAnalysis.h"
#include "Analysis/Concurrency/LockSetAnalysis.h"
#include "Analysis/Concurrency/ThreadAPI.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>

#include <vector>
#include <string>

namespace concurrency {

/**
 * @brief Specialized checker for deadlock detection
 *
 * This class handles the logic for detecting potential deadlocks caused by
 * inconsistent lock acquisition orders that could lead to circular wait conditions.
 */
class DeadlockChecker {
public:
    explicit DeadlockChecker(llvm::Module& module,
                           mhp::LockSetAnalysis* locksetAnalysis,
                           mhp::MHPAnalysis* mhpAnalysis,
                           ThreadAPI* threadAPI);

    /**
     * @brief Check for deadlocks in the module
     * @return Vector of deadlock reports
     */
    std::vector<ConcurrencyBugReport> checkDeadlocks();

private:
    // Analysis components
    llvm::Module& m_module;
    mhp::LockSetAnalysis* m_locksetAnalysis;
    mhp::MHPAnalysis* m_mhpAnalysis;
    ThreadAPI* m_threadAPI;

    // Helper methods for deadlock detection
    bool isLockOperation(const llvm::Instruction* inst) const;
    mhp::LockID getLockID(const llvm::Instruction* inst) const;
    std::vector<std::pair<mhp::LockID, mhp::LockID>> detectLockOrderViolations() const;
    std::string getLockDescription(mhp::LockID lock) const;
    const llvm::Instruction* findMatchingUnlock(const llvm::Instruction* lockInst) const;

    // Deadlock detection logic
};

} // namespace concurrency

#endif // DEADLOCK_CHECKER_H
