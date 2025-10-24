#include "Checker/concurrency/ConcurrencyChecker.h"
#include "Analysis/Concurrency/ThreadAPI.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

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

    // Register bug types with BugReportMgr (Clearblue pattern)
    BugReportMgr& mgr = BugReportMgr::get_instance();
    m_dataRaceTypeId = mgr.register_bug_type("Data Race", BugDescription::BI_HIGH, 
                                              BugDescription::BC_SECURITY, "CWE-362");
    m_deadlockTypeId = mgr.register_bug_type("Deadlock", BugDescription::BI_HIGH,
                                              BugDescription::BC_ERROR, "Deadlock potential");
    m_atomicityViolationTypeId = mgr.register_bug_type("Atomicity Violation", BugDescription::BI_MEDIUM,
                                                        BugDescription::BC_ERROR, "Non-atomic operation sequence");

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

void ConcurrencyChecker::runChecks() {
    if (m_checkDataRaces) {
        checkDataRaces();
    }

    if (m_checkDeadlocks) {
        checkDeadlocks();
    }

    if (m_checkAtomicityViolations) {
        checkAtomicityViolations();
    }
}

void ConcurrencyChecker::checkDataRaces() {
    if (m_dataRaceChecker) {
        auto reports = m_dataRaceChecker->checkDataRaces();
        m_stats.dataRacesFound = reports.size();
        for (const auto& report : reports) {
            reportBug(report, m_dataRaceTypeId);
        }
    }
}

void ConcurrencyChecker::checkDeadlocks() {
    if (m_deadlockChecker) {
        auto reports = m_deadlockChecker->checkDeadlocks();
        m_stats.deadlocksFound = reports.size();
        for (const auto& report : reports) {
            reportBug(report, m_deadlockTypeId);
        }
    }
}

void ConcurrencyChecker::checkAtomicityViolations() {
    if (m_atomicityChecker) {
        auto reports = m_atomicityChecker->checkAtomicityViolations();
        m_stats.atomicityViolationsFound = reports.size();
        for (const auto& report : reports) {
            reportBug(report, m_atomicityViolationTypeId);
        }
    }
}

void ConcurrencyChecker::reportBug(const ConcurrencyBugReport& bug_report, int bug_type_id) {
    // Create a new BugReport following Clearblue pattern
    BugReport* report = new BugReport(bug_type_id);
    
    // Add diagnostic steps showing the concurrency bug trace
    if (bug_report.instruction1) {
        report->append_step(const_cast<Instruction*>(bug_report.instruction1), 
                           "First access: " + bug_report.description);
    }
    
    if (bug_report.instruction2) {
        report->append_step(const_cast<Instruction*>(bug_report.instruction2), 
                           "Second conflicting access");
    }
    
    // Set confidence score based on importance
    report->set_conf_score(bug_report.importance == BugDescription::BI_HIGH ? 90 : 70);
    
    // Report to the manager
    BugReportMgr::get_instance().insert_report(bug_type_id, report);
}

} // namespace concurrency
