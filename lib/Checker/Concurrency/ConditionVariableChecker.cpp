#include "Checker/Concurrency/ConditionVariableChecker.h"
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

ConditionVariableChecker::ConditionVariableChecker(Module& module,
                                                   ThreadAPI* threadAPI,
                                                   LockSetAnalysis* locksetAnalysis)
    : m_module(module), m_threadAPI(threadAPI), m_locksetAnalysis(locksetAnalysis) {}

std::vector<ConcurrencyBugReport> ConditionVariableChecker::checkConditionVariables() {
    std::vector<ConcurrencyBugReport> reports;

    for (Function& func : m_module) {
        if (func.isDeclaration()) continue;

        for (auto& bb : func) {
            for (auto& inst : bb) {
                if (m_threadAPI->isTDCondWait(&inst)) {
                    // 1. Check if wait is called with a mutex
                    const Value* mutex = getMutexForCV(&inst);
                    if (!mutex) {
                        ConcurrencyBugReport report(
                            ConcurrencyBugType::COND_VAR_MISUSE,
                            "Condition variable wait called without a mutex",
                            BugDescription::BI_HIGH, BugDescription::BC_ERROR
                        );
                        report.addStep(&inst, "Wait called here");
                        reports.push_back(report);
                        continue;
                    }

                    // 2. Check if the mutex is actually held at the point of wait
                    if (m_locksetAnalysis) {
                        LockSet mustHeld = m_locksetAnalysis->getMustLockSetAt(&inst);
                        bool isHeld = false;
                        
                        for (auto heldLock : mustHeld) {
                            if (heldLock == mutex || heldLock == mutex->stripPointerCasts()) {
                                isHeld = true;
                                break;
                            }
                        }

                        if (!isHeld) {
                            LockSet mayHeld = m_locksetAnalysis->getMayLockSetAt(&inst);
                            bool mayBeHeld = false;
                            for (auto heldLock : mayHeld) {
                                if (heldLock == mutex || heldLock == mutex->stripPointerCasts()) {
                                    mayBeHeld = true;
                                    break;
                                }
                            }

                            if (!mayBeHeld) {
                                ConcurrencyBugReport report(
                                    ConcurrencyBugType::COND_VAR_MISUSE,
                                    "Mutex not held when calling condition variable wait",
                                    BugDescription::BI_HIGH, BugDescription::BC_ERROR
                                );
                                report.addStep(&inst, "Wait called without holding lock");
                                reports.push_back(report);
                            } else {
                                ConcurrencyBugReport report(
                                    ConcurrencyBugType::COND_VAR_MISUSE,
                                    "Mutex might not be held when calling condition variable wait",
                                    BugDescription::BI_MEDIUM, BugDescription::BC_WARNING
                                );
                                report.addStep(&inst, "Wait called here");
                                reports.push_back(report);
                            }
                        }
                    }
                }
            }
        }
    }
    
    return reports;
}

const Value* ConditionVariableChecker::getMutexForCV(const Instruction* waitInst) const {
    return m_threadAPI->getCondMutex(waitInst);
}

std::string ConditionVariableChecker::getInstructionLocation(const Instruction* inst) const {
    std::string location;
    raw_string_ostream os(location);
    if (const Function* func = inst->getFunction())
        os << func->getName();
    if (const BasicBlock* bb = inst->getParent())
        os << ":" << bb->getName();
    return os.str();
}

} // namespace concurrency
