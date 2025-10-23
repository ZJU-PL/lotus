#include "Checker/concurrency/AtomicityChecker.h"

#include <llvm/Support/raw_ostream.h>

#include <algorithm>

using namespace llvm;
using namespace mhp;

namespace concurrency {

AtomicityChecker::AtomicityChecker(Module& module,
                                 MHPAnalysis* mhpAnalysis,
                                 LockSetAnalysis* locksetAnalysis,
                                 ThreadAPI* threadAPI)
    : m_module(module), m_mhpAnalysis(mhpAnalysis),
      m_locksetAnalysis(locksetAnalysis), m_threadAPI(threadAPI) {}

std::vector<ConcurrencyBugReport> AtomicityChecker::checkAtomicityViolations() {
    std::vector<ConcurrencyBugReport> reports;

    // Look for potential atomicity violations in critical sections
    for (Function& func : m_module) {
        if (func.isDeclaration()) continue;

        for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
            const Instruction* inst = &*I;

            // Look for lock acquire instructions
            if (isLockOperation(inst) && m_threadAPI->isTDAcquire(inst)) {
                // This is the start of a critical section
                const Instruction* lockInst = inst;

                // Find the corresponding unlock
                const Instruction* unlockInst = findMatchingUnlock(lockInst);

                if (unlockInst) {
                    // Check for operations within the critical section that might be atomicity violations
                    checkCriticalSectionForAtomicityViolations(lockInst, unlockInst, reports);
                }
            }
        }
    }

    return reports;
}

void AtomicityChecker::checkCriticalSectionForAtomicityViolations(
    const Instruction* lockInst, const Instruction* unlockInst,
    std::vector<ConcurrencyBugReport>& reports) {

    // This is a simplified implementation
    // In practice, this would need more sophisticated analysis

    // For now, we'll flag any non-atomic operations in critical sections
    // that could potentially be problematic

    const BasicBlock* lockBB = lockInst->getParent();
    const BasicBlock* unlockBB = unlockInst->getParent();

    // If lock and unlock are in different basic blocks, we need to be more careful
    // For simplicity, we'll assume they're in the same function for now

    if (lockBB->getParent() != unlockBB->getParent()) {
        return; // Skip cross-function analysis for now
    }

    // Find instructions between lock and unlock
    bool inCriticalSection = false;
    const Function* func = lockBB->getParent();

    for (const BasicBlock& bb : *func) {
        for (const Instruction& inst : bb) {
            if (&inst == lockInst) {
                inCriticalSection = true;
                continue;
            }

            if (&inst == unlockInst) {
                inCriticalSection = false;
                continue;
            }

            if (inCriticalSection && isMemoryAccess(&inst)) {
                // Check if this memory access might be interleaved with operations from other threads
                // This is a simplified check - in practice, we'd need more sophisticated analysis

                if (!isAtomicOperation(&inst)) {
                    // This could be an atomicity violation if the operation should be atomic
                    std::string description = "Potential atomicity violation: non-atomic operation in critical section";
                    description += " at ";
                    description += getInstructionLocation(&inst);

                    reports.emplace_back(
                        ConcurrencyBugType::ATOMICITY_VIOLATION,
                        lockInst, &inst, description,
                        BugDescription::BI_MEDIUM,
                        BugDescription::BC_WARNING
                    );
                }
            }
        }
    }
}

bool AtomicityChecker::isAtomicSequence(const Instruction* start, const Instruction* end) const {
    // Simplified implementation - in practice this would be more sophisticated
    // For now, assume sequences are atomic if they're simple operations
    return false;
}

bool AtomicityChecker::mayBeInterleaved(const Instruction* inst1, const Instruction* inst2) const {
    // Check if two instructions from different threads may be interleaved
    // This is a simplified check - in practice, this would need more sophisticated analysis
    return m_mhpAnalysis->mayHappenInParallel(inst1, inst2);
}

bool AtomicityChecker::isLockOperation(const Instruction* inst) const {
    return m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst);
}

mhp::LockID AtomicityChecker::getLockID(const Instruction* inst) const {
    return m_threadAPI->getLockVal(inst);
}

std::string AtomicityChecker::getInstructionLocation(const Instruction* inst) const {
    std::string location;
    raw_string_ostream os(location);

    if (const Function* func = inst->getFunction()) {
        os << func->getName();
    }

    if (const BasicBlock* bb = inst->getParent()) {
        os << ":" << bb->getName();
    }

    return os.str();
}

const Instruction* AtomicityChecker::findMatchingUnlock(const Instruction* lockInst) const {
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

bool AtomicityChecker::isMemoryAccess(const Instruction* inst) const {
    return isa<LoadInst>(inst) || isa<StoreInst>(inst) ||
           isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

bool AtomicityChecker::isAtomicOperation(const Instruction* inst) const {
    return isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

} // namespace concurrency
