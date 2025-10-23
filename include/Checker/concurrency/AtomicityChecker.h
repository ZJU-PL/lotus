#ifndef ATOMICITY_CHECKER_H
#define ATOMICITY_CHECKER_H

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
 * @brief Specialized checker for atomicity violation detection
 *
 * This class handles the logic for detecting potential atomicity violations
 * where operations that should be atomic may be interleaved with other operations
 * from different threads.
 */
class AtomicityChecker {
public:
    explicit AtomicityChecker(llvm::Module& module,
                            mhp::MHPAnalysis* mhpAnalysis,
                            mhp::LockSetAnalysis* locksetAnalysis,
                            ThreadAPI* threadAPI);

    /**
     * @brief Check for atomicity violations in the module
     * @return Vector of atomicity violation reports
     */
    std::vector<ConcurrencyBugReport> checkAtomicityViolations();

private:
    // Analysis components
    llvm::Module& m_module;
    mhp::MHPAnalysis* m_mhpAnalysis;
    mhp::LockSetAnalysis* m_locksetAnalysis;
    ThreadAPI* m_threadAPI;

    // Helper methods for atomicity violation detection
    bool isLockOperation(const llvm::Instruction* inst) const;
    mhp::LockID getLockID(const llvm::Instruction* inst) const;
    std::string getInstructionLocation(const llvm::Instruction* inst) const;
    const llvm::Instruction* findMatchingUnlock(const llvm::Instruction* lockInst) const;
    bool isMemoryAccess(const llvm::Instruction* inst) const;
    bool isAtomicOperation(const llvm::Instruction* inst) const;

    // Atomicity violation detection logic
    void checkCriticalSectionForAtomicityViolations(
        const llvm::Instruction* lockInst, const llvm::Instruction* unlockInst,
        std::vector<ConcurrencyBugReport>& reports);
    bool isAtomicSequence(const llvm::Instruction* start, const llvm::Instruction* end) const;
    bool mayBeInterleaved(const llvm::Instruction* inst1, const llvm::Instruction* inst2) const;
};

} // namespace concurrency

#endif // ATOMICITY_CHECKER_H
