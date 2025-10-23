#include "Checker/concurrency/DeadlockChecker.h"

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

        std::string description = "Potential deadlock: inconsistent lock acquisition order between ";
        description += getLockDescription(lock1);
        description += " and ";
        description += getLockDescription(lock2);

        // Find representative instructions for these locks
        const Instruction* inst1 = nullptr;
        const Instruction* inst2 = nullptr;

        // Find instructions that acquire these locks
        auto lockAcquires1 = m_locksetAnalysis->getLockAcquires(lock1);
        auto lockAcquires2 = m_locksetAnalysis->getLockAcquires(lock2);

        if (!lockAcquires1.empty() && !lockAcquires2.empty()) {
            inst1 = lockAcquires1[0];
            inst2 = lockAcquires2[0];
        }

        reports.emplace_back(
            ConcurrencyBugType::DEADLOCK,
            inst1, inst2, description,
            BugDescription::BI_HIGH,
            BugDescription::BC_ERROR
        );
    }

    return reports;
}

std::vector<std::pair<mhp::LockID, mhp::LockID>> DeadlockChecker::detectLockOrderViolations() const {
    return m_locksetAnalysis->detectLockOrderInversions();
}

std::string DeadlockChecker::getLockDescription(mhp::LockID lock) const {
    std::string desc;
    raw_string_ostream os(desc);
    lock->printAsOperand(os, false);
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
