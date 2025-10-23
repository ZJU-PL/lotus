#ifndef CONCURRENCY_CHECKER_H
#define CONCURRENCY_CHECKER_H

#include "Analysis/Concurrency/MHPAnalysis.h"
#include "Analysis/Concurrency/LockSetAnalysis.h"
#include "Checker/concurrency/ConcurrencyBugReport.h"
#include "Checker/concurrency/DataRaceChecker.h"
#include "Checker/concurrency/DeadlockChecker.h"
#include "Checker/concurrency/AtomicityChecker.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Analysis/AliasAnalysis.h>

#include <unordered_set>
//#include <unordered_map>
#include <vector>
#include <string>
#include <memory>

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

    // Specialized checker components
    std::unique_ptr<DataRaceChecker> m_dataRaceChecker;
    std::unique_ptr<DeadlockChecker> m_deadlockChecker;
    std::unique_ptr<AtomicityChecker> m_atomicityChecker;

    // Configuration
    bool m_checkDataRaces = true;
    bool m_checkDeadlocks = true;
    bool m_checkAtomicityViolations = true;

    // Results tracking
    Statistics m_stats;
};

} // namespace concurrency

#endif // CONCURRENCY_CHECKER_H
