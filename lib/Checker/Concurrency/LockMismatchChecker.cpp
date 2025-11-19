#include "Checker/Concurrency/LockMismatchChecker.h"
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/InstIterator.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

LockMismatchChecker::LockMismatchChecker(Module& module,
                                         LockSetAnalysis* locksetAnalysis,
                                         ThreadAPI* threadAPI)
    : m_module(module), m_locksetAnalysis(locksetAnalysis), m_threadAPI(threadAPI) {}

std::vector<ConcurrencyBugReport> LockMismatchChecker::checkLockMisuse() {
    std::vector<ConcurrencyBugReport> reports;

    if (!m_locksetAnalysis) return reports;

    for (Function& func : m_module) {
        if (func.isDeclaration()) continue;

        for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
            Instruction* inst = &*I;

            if (m_threadAPI->isTDRelease(inst)) {
                // Check for Unlock without Lock
                LockID lock = m_threadAPI->getLockVal(inst);
                if (!lock) continue;
                lock = lock->stripPointerCasts();

                if (!m_locksetAnalysis->mustHoldLock(inst, lock)) {
                    if (m_locksetAnalysis->mayHoldLock(inst, lock)) {
                         ConcurrencyBugReport report(
                            ConcurrencyBugType::LOCK_MISMATCH,
                            "Potential unlock without guarantee of holding lock",
                            BugDescription::BI_MEDIUM, BugDescription::BC_WARNING
                        );
                        report.addStep(inst, "Unlock operation");
                        reports.push_back(report);
                    } else {
                        ConcurrencyBugReport report(
                            ConcurrencyBugType::LOCK_MISMATCH,
                            "Unlock called without holding the lock",
                            BugDescription::BI_HIGH, BugDescription::BC_ERROR
                        );
                        report.addStep(inst, "Unlock operation");
                        reports.push_back(report);
                    }
                }
            } else if (m_threadAPI->isTDAcquire(inst)) {
                // Check for Double Lock
                LockID lock = m_threadAPI->getLockVal(inst);
                if (!lock) continue;
                lock = lock->stripPointerCasts();

                if (m_locksetAnalysis->mustHoldLock(inst, lock)) {
                     ConcurrencyBugReport report(
                        ConcurrencyBugType::LOCK_MISMATCH,
                        "Double lock: attempting to acquire a lock already held",
                        BugDescription::BI_HIGH, BugDescription::BC_ERROR
                    );
                    report.addStep(inst, "Second lock acquisition");
                    reports.push_back(report);
                }
            }
        }

        // Check for Lock Leaks at return points
        for (auto& bb : func) {
            if (isa<ReturnInst>(bb.getTerminator())) {
                const Instruction* term = bb.getTerminator();
                LockSet heldLocks = m_locksetAnalysis->getMustLockSetAt(term);
                
                if (!heldLocks.empty()) {
                    bool intentional = false;
                    StringRef funcName = func.getName();
                    if (funcName.contains("lock") || funcName.contains("Lock") || 
                        funcName.contains("acquire") || funcName.contains("Acquire")) {
                        intentional = true;
                    }

                    if (!intentional) {
                         for (auto lock : heldLocks) {
                             ConcurrencyBugReport report(
                                ConcurrencyBugType::LOCK_MISMATCH,
                                "Lock leak: function returns with lock held",
                                BugDescription::BI_MEDIUM, BugDescription::BC_WARNING
                            );
                            report.addStep(term, "Function return");
                            reports.push_back(report);
                         }
                    }
                }
            }
        }
    }

    return reports;
}

std::string LockMismatchChecker::getInstructionLocation(const Instruction* inst) const {
    std::string location;
    raw_string_ostream os(location);
    if (const Function* func = inst->getFunction())
        os << func->getName();
    if (const BasicBlock* bb = inst->getParent())
        os << ":" << bb->getName();
    return os.str();
}

} // namespace concurrency
