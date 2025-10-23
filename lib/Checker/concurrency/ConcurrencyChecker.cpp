#include "Checker/concurrency/ConcurrencyChecker.h"
#include "Analysis/Concurrency/ThreadAPI.h"

#include <llvm/IR/Constants.h>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

//#include <algorithm>
//#include <sstream>

using namespace llvm;
using namespace mhp;

namespace concurrency {

ConcurrencyChecker::ConcurrencyChecker(Module& module)
    : m_module(module), m_aliasAnalysis(nullptr), m_threadAPI(ThreadAPI::getThreadAPI()) {

    // Initialize analyses
    m_mhpAnalysis = std::make_unique<MHPAnalysis>(module);
    m_mhpAnalysis->enableLockSetAnalysis();
    m_mhpAnalysis->analyze();

    m_locksetAnalysis = std::make_unique<LockSetAnalysis>(module);
    m_locksetAnalysis->analyze();

    // Initialize specialized checkers
    m_dataRaceChecker = std::make_unique<DataRaceChecker>(module, m_mhpAnalysis.get(), m_aliasAnalysis);
    m_deadlockChecker = std::make_unique<DeadlockChecker>(module, m_locksetAnalysis.get(),
                                                         m_mhpAnalysis.get(), m_threadAPI);
    m_atomicityChecker = std::make_unique<AtomicityChecker>(module, m_mhpAnalysis.get(),
                                                           m_locksetAnalysis.get(), m_threadAPI);

    // Collect statistics
    m_stats.totalInstructions = 0;
    m_stats.mhpPairs = m_mhpAnalysis->getStatistics().num_mhp_pairs;
    m_stats.locksAnalyzed = m_locksetAnalysis->getStatistics().num_locks;

    // Count total instructions
    for (Function& func : module) {
        if (!func.isDeclaration()) {
            for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
                m_stats.totalInstructions++;
            }
        }
    }
}

std::vector<ConcurrencyBugReport> ConcurrencyChecker::runChecks() {
    std::vector<ConcurrencyBugReport> allReports;

    if (m_checkDataRaces) {
        auto raceReports = checkDataRaces();
        allReports.insert(allReports.end(), raceReports.begin(), raceReports.end());
        m_stats.dataRacesFound = raceReports.size();
    }

    if (m_checkDeadlocks) {
        auto deadlockReports = checkDeadlocks();
        allReports.insert(allReports.end(), deadlockReports.begin(), deadlockReports.end());
        m_stats.deadlocksFound = deadlockReports.size();
    }

    if (m_checkAtomicityViolations) {
        auto atomicityReports = checkAtomicityViolations();
        allReports.insert(allReports.end(), atomicityReports.begin(), atomicityReports.end());
        m_stats.atomicityViolationsFound = atomicityReports.size();
    }

    return allReports;
}

std::vector<ConcurrencyBugReport> ConcurrencyChecker::checkDataRaces() {
    if (m_dataRaceChecker) {
        return m_dataRaceChecker->checkDataRaces();
    }
    return {};
}

std::vector<ConcurrencyBugReport> ConcurrencyChecker::checkDeadlocks() {
    if (m_deadlockChecker) {
        return m_deadlockChecker->checkDeadlocks();
    }
    return {};
}

std::vector<ConcurrencyBugReport> ConcurrencyChecker::checkAtomicityViolations() {
    if (m_atomicityChecker) {
        return m_atomicityChecker->checkAtomicityViolations();
    }
    return {};
}

} // namespace concurrency
