#ifndef CONCURRENCY_CHECKER_H
#define CONCURRENCY_CHECKER_H

#include "Analysis/Concurrency/MHPAnalysis.h"
#include "Analysis/Concurrency/LockSetAnalysis.h"
#include "Checker/Report/BugTypes.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Analysis/AliasAnalysis.h>

#include <unordered_set>
//#include <unordered_map>
#include <vector>
#include <string>

namespace concurrency {

enum class ConcurrencyBugType {
    DATA_RACE,
    DEADLOCK,
    ATOMICITY_VIOLATION
};

struct ConcurrencyBugReport {
    ConcurrencyBugType bugType;
    const llvm::Instruction* instruction1;
    const llvm::Instruction* instruction2;
    std::string description;
    std::string location;
    BugDescription::BugImportance importance;
    BugDescription::BugClassification classification;

    ConcurrencyBugReport(ConcurrencyBugType type,
                        const llvm::Instruction* inst1,
                        const llvm::Instruction* inst2,
                        const std::string& desc,
                        BugDescription::BugImportance imp = BugDescription::BI_HIGH,
                        BugDescription::BugClassification cls = BugDescription::BC_ERROR)
        : bugType(type), instruction1(inst1), instruction2(inst2),
          description(desc), importance(imp), classification(cls) {}
};

/**
 * @brief Static checker for concurrency problems including data races, deadlocks, and atomicity violations
 *
 * This checker uses MHP analysis and lock set analysis to detect:
 * - Data races: concurrent accesses to shared variables without proper synchronization
 * - Deadlocks: potential cycles in lock acquisition order
 * - Atomicity violations: operations that should be atomic but may be interleaved
 */
class ConcurrencyChecker {
public:
    explicit ConcurrencyChecker(llvm::Module& module);
    ~ConcurrencyChecker() = default;

    /**
     * @brief Run all concurrency checks
     * @return Vector of detected concurrency bugs
     */
    std::vector<ConcurrencyBugReport> runChecks();

    /**
     * @brief Check for data races
     * @return Vector of data race reports
     */
    std::vector<ConcurrencyBugReport> checkDataRaces();

    /**
     * @brief Check for deadlocks
     * @return Vector of deadlock reports
     */
    std::vector<ConcurrencyBugReport> checkDeadlocks();

    /**
     * @brief Check for atomicity violations
     * @return Vector of atomicity violation reports
     */
    std::vector<ConcurrencyBugReport> checkAtomicityViolations();

    /**
     * @brief Set alias analysis for better precision
     */
    void setAliasAnalysis(llvm::AAResults* aa) { m_aliasAnalysis = aa; }

    /**
     * @brief Enable/disable specific checks
     */
    void enableDataRaceCheck(bool enable) { m_checkDataRaces = enable; }
    void enableDeadlockCheck(bool enable) { m_checkDeadlocks = enable; }
    void enableAtomicityCheck(bool enable) { m_checkAtomicityViolations = enable; }

    /**
     * @brief Get statistics about the analysis
     */
    struct Statistics {
        size_t totalInstructions;
        size_t mhpPairs;
        size_t locksAnalyzed;
        size_t dataRacesFound;
        size_t deadlocksFound;
        size_t atomicityViolationsFound;
    };

    Statistics getStatistics() const { return m_stats; }

private:
    // Analysis components
    llvm::Module& m_module;
    std::unique_ptr<mhp::MHPAnalysis> m_mhpAnalysis;
    std::unique_ptr<mhp::LockSetAnalysis> m_locksetAnalysis;
    llvm::AAResults* m_aliasAnalysis;
    ThreadAPI* m_threadAPI;

    // Configuration
    bool m_checkDataRaces = true;
    bool m_checkDeadlocks = true;
    bool m_checkAtomicityViolations = true;

    // Results tracking
    Statistics m_stats;
    std::unordered_set<const llvm::Value*> m_sharedVariables;

    // Helper methods for data race detection
    bool mayAlias(const llvm::Value* v1, const llvm::Value* v2) const;
    bool isMemoryAccess(const llvm::Instruction* inst) const;
    bool isWriteAccess(const llvm::Instruction* inst) const;
    bool isAtomicOperation(const llvm::Instruction* inst) const;
    std::string getInstructionLocation(const llvm::Instruction* inst) const;

    // Helper methods for deadlock detection
    bool isLockOperation(const llvm::Instruction* inst) const;
    mhp::LockID getLockID(const llvm::Instruction* inst) const;
    std::vector<std::pair<mhp::LockID, mhp::LockID>> detectLockOrderViolations() const;

    // Helper methods for atomicity violation detection
    bool isAtomicSequence(const llvm::Instruction* start, const llvm::Instruction* end) const;
    bool mayBeInterleaved(const llvm::Instruction* inst1, const llvm::Instruction* inst2) const;

    // Additional helper methods
    const llvm::Value* getMemoryLocation(const llvm::Instruction* inst) const;
    std::string getInstructionDescription(const llvm::Instruction* inst) const;
    std::string getLockDescription(mhp::LockID lock) const;
    const llvm::Instruction* findMatchingUnlock(const llvm::Instruction* lockInst) const;
    void checkCriticalSectionForAtomicityViolations(
        const llvm::Instruction* lockInst, const llvm::Instruction* unlockInst,
        std::vector<ConcurrencyBugReport>& reports);
};

} // namespace concurrency

#endif // CONCURRENCY_CHECKER_H
