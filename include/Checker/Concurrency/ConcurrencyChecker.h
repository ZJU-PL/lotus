#ifndef CONCURRENCY_CHECKER_H
#define CONCURRENCY_CHECKER_H

#include "Analysis/Concurrency/MHPAnalysis.h"
#include "Analysis/Concurrency/LockSetAnalysis.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Checker/Concurrency/DataRaceChecker.h"
#include "Checker/Concurrency/DeadlockChecker.h"
#include "Checker/Concurrency/AtomicityChecker.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>

#include <unordered_set>
#include <vector>
#include <string>
#include <memory>

namespace lotus {
class AliasAnalysisWrapper;
}

namespace concurrency {

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
     * @brief Run all concurrency checks and report bugs to BugReportMgr
     */
    void runChecks();

    /**
     * @brief Check for data races and report to BugReportMgr
     */
    void checkDataRaces();

    /**
     * @brief Check for deadlocks and report to BugReportMgr
     */
    void checkDeadlocks();

    /**
     * @brief Check for atomicity violations and report to BugReportMgr
     */
    void checkAtomicityViolations();

    /**
     * @brief Set alias analysis wrapper for better precision
     */
    void setAliasAnalysis(lotus::AliasAnalysisWrapper* aa) { m_aliasAnalysis = aa; }

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
    lotus::AliasAnalysisWrapper* m_aliasAnalysis;
    ThreadAPI* m_threadAPI;

    // Specialized checker components
    std::unique_ptr<DataRaceChecker> m_dataRaceChecker;
    std::unique_ptr<DeadlockChecker> m_deadlockChecker;
    std::unique_ptr<AtomicityChecker> m_atomicityChecker;

    // Configuration
    bool m_checkDataRaces = true;
    bool m_checkDeadlocks = true;
    bool m_checkAtomicityViolations = true;

    // Bug type IDs (registered with BugReportMgr)
    int m_dataRaceTypeId;
    int m_deadlockTypeId;
    int m_atomicityViolationTypeId;

    // Results tracking
    Statistics m_stats;
    
    // Helper to convert ConcurrencyBugReport to BugReport
    void reportBug(const ConcurrencyBugReport& bug_report, int bug_type_id);
};

} // namespace concurrency

#endif // CONCURRENCY_CHECKER_H
