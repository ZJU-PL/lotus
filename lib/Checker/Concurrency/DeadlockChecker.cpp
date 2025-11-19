#include "Checker/Concurrency/DeadlockChecker.h"

#include <llvm/Support/raw_ostream.h>

//#include <algorithm>

using namespace llvm;
using namespace mhp;

namespace concurrency {

DeadlockChecker::DeadlockChecker(Module& module,
                               LockSetAnalysis* locksetAnalysis,
                               MHPAnalysis* mhpAnalysis,
                               ThreadAPI* threadAPI)
    : m_module(module), m_locksetAnalysis(locksetAnalysis),
      m_mhpAnalysis(mhpAnalysis), m_threadAPI(threadAPI) {}

std::vector<ConcurrencyBugReport> DeadlockChecker::checkDeadlocks() {
    std::vector<ConcurrencyBugReport> reports;

    auto lockOrderViolations = detectLockOrderViolations();

    for (const auto& violation : lockOrderViolations) {
        mhp::LockID lock1 = violation.first;
        mhp::LockID lock2 = violation.second;

        // Find instructions that acquire these locks
        auto lockAcquires1 = m_locksetAnalysis->getLockAcquires(lock1);
        auto lockAcquires2 = m_locksetAnalysis->getLockAcquires(lock2);
        
        if (lockAcquires1.empty() || lockAcquires2.empty()) continue;

        // Check if threads using these locks can run in parallel
        bool canRunInParallel = false;
        const Instruction* inst1 = nullptr;
        const Instruction* inst2 = nullptr;

        // Check all pairs of acquires to see if any can happen in parallel
        for (const auto* a1 : lockAcquires1) {
            for (const auto* a2 : lockAcquires2) {
                if (m_mhpAnalysis->mayHappenInParallel(a1, a2)) {
                    canRunInParallel = true;
                    inst1 = a1;
                    inst2 = a2;
                    break;
                }
            }
            if (canRunInParallel) break;
        }

        if (!canRunInParallel) continue;

        std::string description = "Potential deadlock: inconsistent lock acquisition order between ";
        description += getLockDescription(lock1);
        description += " and ";
        description += getLockDescription(lock2);
        description += ". Threads acquiring these locks may run in parallel.";

        ConcurrencyBugReport report(
            ConcurrencyBugType::DEADLOCK,
            description,
            BugDescription::BI_HIGH,
            BugDescription::BC_ERROR
        );

        if (inst1) report.addStep(inst1, "Lock 1 acquisition");
        if (inst2) report.addStep(inst2, "Lock 2 acquisition");
        
        reports.push_back(report);
    }

    return reports;
}

std::vector<std::pair<mhp::LockID, mhp::LockID>> DeadlockChecker::detectLockOrderViolations() const {
    return m_locksetAnalysis->detectLockOrderInversions();
}

std::string DeadlockChecker::getLockDescription(mhp::LockID lock) const {
    std::string desc;
    raw_string_ostream os(desc);
    if (lock && lock->hasName())
        os << lock->getName();
    else
        os << *lock;
    return os.str();
}

bool DeadlockChecker::isLockOperation(const Instruction* inst) const {
    return m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst);
}

mhp::LockID DeadlockChecker::getLockID(const Instruction* inst) const {
    return m_threadAPI->getLockVal(inst);
}

const Instruction* DeadlockChecker::findMatchingUnlock(const Instruction* lockInst) const {
    if (!lockInst) return nullptr;

    mhp::LockID lock = getLockID(lockInst);
    if (!lock) return nullptr;

    auto releases = m_locksetAnalysis->getLockReleases(lock);

    // Find the next release after this acquire
    for (const Instruction* release : releases) {
        if (m_mhpAnalysis->mustPrecede(lockInst, release)) {
            return release;
        }
    }

    return nullptr;
}

} // namespace concurrency
