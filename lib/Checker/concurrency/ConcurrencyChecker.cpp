#include "Checker/concurrency/ConcurrencyChecker.h"
#include "Analysis/Concurrency/ThreadAPI.h"

#include <llvm/IR/Constants.h>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <sstream>

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
    std::vector<ConcurrencyBugReport> reports;

    // Collect all memory access instructions grouped by variable
    std::unordered_map<const Value*, std::vector<const Instruction*>> variableAccesses;

    for (Function& func : m_module) {
        if (func.isDeclaration()) continue;

        for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
            const Instruction* inst = &*I;

            if (isMemoryAccess(inst)) {
                // Get the memory location being accessed
                const Value* memLocation = nullptr;

                if (const LoadInst* load = dyn_cast<LoadInst>(inst)) {
                    memLocation = load->getPointerOperand();
                } else if (const StoreInst* store = dyn_cast<StoreInst>(inst)) {
                    memLocation = store->getPointerOperand();
                } else if (const AtomicRMWInst* rmw = dyn_cast<AtomicRMWInst>(inst)) {
                    memLocation = rmw->getPointerOperand();
                } else if (const AtomicCmpXchgInst* cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst)) {
                    memLocation = cmpxchg->getPointerOperand();
                }

                if (memLocation) {
                    variableAccesses[memLocation].push_back(inst);
                }
            }
        }
    }

    // Check for data races between accesses to the same variable
    for (const auto& pair : variableAccesses) {
        const auto& accesses = pair.second;

        // Skip if only one access or atomic operations
        if (accesses.size() < 2) continue;

        // Check all pairs of accesses
        for (size_t i = 0; i < accesses.size(); ++i) {
            const Instruction* inst1 = accesses[i];

            // Skip atomic operations for data race detection
            if (isAtomicOperation(inst1)) continue;

            for (size_t j = i + 1; j < accesses.size(); ++j) {
                const Instruction* inst2 = accesses[j];

                // Skip if both are atomic operations
                if (isAtomicOperation(inst1) && isAtomicOperation(inst2)) continue;

                // Check if they may happen in parallel (MHP)
                if (m_mhpAnalysis->mayHappenInParallel(inst1, inst2)) {
                    // Check if at least one is a write
                    bool hasWrite = isWriteAccess(inst1) || isWriteAccess(inst2);

                    if (hasWrite) {
                        // Check if they access the same memory location (considering aliases)
                        if (mayAlias(getMemoryLocation(inst1), getMemoryLocation(inst2))) {
                            std::string description = "Potential data race between ";
                            description += getInstructionLocation(inst1);
                            description += " and ";
                            description += getInstructionLocation(inst2);

                            reports.emplace_back(
                                ConcurrencyBugType::DATA_RACE,
                                inst1, inst2, description,
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR
                            );
                        }
                    }
                }
            }
        }
    }

    return reports;
}

std::vector<ConcurrencyBugReport> ConcurrencyChecker::checkDeadlocks() {
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

std::vector<ConcurrencyBugReport> ConcurrencyChecker::checkAtomicityViolations() {
    std::vector<ConcurrencyBugReport> reports;

    // For atomicity violations, we look for sequences of operations that should be atomic
    // but might be interleaved with other operations

    // This is a simplified implementation - in practice, this would need more sophisticated
    // analysis to identify atomic sequences

    // For now, we'll check for potential atomicity violations in critical sections
    // where operations might be interleaved

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

// Helper methods implementation

bool ConcurrencyChecker::mayAlias(const Value* v1, const Value* v2) const {
    if (!v1 || !v2) return false;
    if (v1 == v2) return true;

    if (m_aliasAnalysis) {
        MemoryLocation loc1 = MemoryLocation::getBeforeOrAfter(v1);
        MemoryLocation loc2 = MemoryLocation::getBeforeOrAfter(v2);
        return m_aliasAnalysis->alias(loc1, loc2) != AliasResult::NoAlias;
    }

    // Conservative: assume may alias if we can't prove otherwise
    return true;
}

bool ConcurrencyChecker::isMemoryAccess(const Instruction* inst) const {
    return isa<LoadInst>(inst) || isa<StoreInst>(inst) ||
           isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

bool ConcurrencyChecker::isWriteAccess(const Instruction* inst) const {
    return isa<StoreInst>(inst) || isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

bool ConcurrencyChecker::isAtomicOperation(const Instruction* inst) const {
    return isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

const Value* ConcurrencyChecker::getMemoryLocation(const Instruction* inst) const {
    if (const LoadInst* load = dyn_cast<LoadInst>(inst)) {
        return load->getPointerOperand();
    } else if (const StoreInst* store = dyn_cast<StoreInst>(inst)) {
        return store->getPointerOperand();
    } else if (const AtomicRMWInst* rmw = dyn_cast<AtomicRMWInst>(inst)) {
        return rmw->getPointerOperand();
    } else if (const AtomicCmpXchgInst* cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst)) {
        return cmpxchg->getPointerOperand();
    }
    return nullptr;
}

std::string ConcurrencyChecker::getInstructionDescription(const Instruction* inst) const {
    std::string desc;
    raw_string_ostream os(desc);
    inst->print(os);
    return os.str();
}

std::string ConcurrencyChecker::getInstructionLocation(const Instruction* inst) const {
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

bool ConcurrencyChecker::isLockOperation(const Instruction* inst) const {
    return m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst);
}

mhp::LockID ConcurrencyChecker::getLockID(const Instruction* inst) const {
    return m_threadAPI->getLockVal(inst);
}

std::vector<std::pair<mhp::LockID, mhp::LockID>> ConcurrencyChecker::detectLockOrderViolations() const {
    return m_locksetAnalysis->detectLockOrderInversions();
}

std::string ConcurrencyChecker::getLockDescription(mhp::LockID lock) const {
    std::string desc;
    raw_string_ostream os(desc);
    lock->printAsOperand(os, false);
    return os.str();
}

const Instruction* ConcurrencyChecker::findMatchingUnlock(const Instruction* lockInst) const {
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

void ConcurrencyChecker::checkCriticalSectionForAtomicityViolations(
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

} // namespace concurrency
