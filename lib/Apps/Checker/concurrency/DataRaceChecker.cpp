#include "Apps/Checker/concurrency/DataRaceChecker.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

DataRaceChecker::DataRaceChecker(Module& module, MHPAnalysis* mhpAnalysis,
                                 AliasAnalysisWrapper* aliasAnalysis)
    : m_module(module), m_mhpAnalysis(mhpAnalysis), m_aliasAnalysis(aliasAnalysis) {}

// Detects data races by checking all pairs of memory accesses.
// A data race occurs when:
//   1. Two instructions may happen in parallel (MHP analysis)
//   2. At least one is a write operation
//   3. They may access the same memory location (alias analysis)
//   4. Neither operation is atomic
std::vector<ConcurrencyBugReport> DataRaceChecker::checkDataRaces() {
    std::vector<ConcurrencyBugReport> reports;
    std::unordered_map<const Value*, std::vector<const Instruction*>> variableAccesses;
    collectVariableAccesses(variableAccesses);

    // Check all pairs of accesses to potentially aliased memory locations.
    for (const auto& pair : variableAccesses) {
        const auto& accesses = pair.second;
        if (accesses.size() < 2) continue;

        for (size_t i = 0; i < accesses.size(); ++i) {
            const Instruction* inst1 = accesses[i];
            if (isAtomicOperation(inst1)) continue;  // Atomic operations prevent races.

            for (size_t j = i + 1; j < accesses.size(); ++j) {
                const Instruction* inst2 = accesses[j];
                if (isAtomicOperation(inst2)) continue;

                // Report race if: concurrent + conflicting + aliased.
                if (m_mhpAnalysis->mayHappenInParallel(inst1, inst2) &&
                    (isWriteAccess(inst1) || isWriteAccess(inst2)) &&
                    mayAccessSameLocation(inst1, inst2)) {
                    
                    reports.emplace_back(
                        ConcurrencyBugType::DATA_RACE, inst1, inst2,
                        "Potential data race between " + getInstructionLocation(inst1) +
                        " and " + getInstructionLocation(inst2),
                        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
                }
            }
        }
    }
    return reports;
}

// Collects all memory access instructions, grouped by the memory location they access.
// This allows efficient pairwise comparison of accesses to the same location.
void DataRaceChecker::collectVariableAccesses(
    std::unordered_map<const Value*, std::vector<const Instruction*>>& variableAccesses) {
    for (Function& func : m_module) {
        if (func.isDeclaration()) continue;
        for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
            if (isMemoryAccess(&*I)) {
                if (const Value* memLoc = getMemoryLocation(&*I))
                    variableAccesses[memLoc].push_back(&*I);
            }
        }
    }
}

// Checks if two instructions may access the same memory location using alias analysis.
bool DataRaceChecker::mayAccessSameLocation(const Instruction* inst1,
                                            const Instruction* inst2) const {
    return mayAlias(getMemoryLocation(inst1), getMemoryLocation(inst2));
}

// Returns true if two values may alias (point to overlapping memory).
// Uses alias analysis wrapper when available, otherwise conservatively assumes aliasing.
bool DataRaceChecker::mayAlias(const Value* v1, const Value* v2) const {
    if (!v1 || !v2) return false;
    if (v1 == v2) return true;
    if (m_aliasAnalysis) {
        return m_aliasAnalysis->mayAlias(v1, v2);
    }
    return true;  // Conservative: assume may alias if we can't prove otherwise.
}

bool DataRaceChecker::isMemoryAccess(const Instruction* inst) const {
    return isa<LoadInst>(inst) || isa<StoreInst>(inst) ||
           isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

bool DataRaceChecker::isWriteAccess(const Instruction* inst) const {
    return isa<StoreInst>(inst) || isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

bool DataRaceChecker::isAtomicOperation(const Instruction* inst) const {
    return isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

// Extracts the memory location (pointer operand) from a memory access instruction.
const Value* DataRaceChecker::getMemoryLocation(const Instruction* inst) const {
    if (const auto* load = dyn_cast<LoadInst>(inst))
        return load->getPointerOperand();
    if (const auto* store = dyn_cast<StoreInst>(inst))
        return store->getPointerOperand();
    if (const auto* rmw = dyn_cast<AtomicRMWInst>(inst))
        return rmw->getPointerOperand();
    if (const auto* cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst))
        return cmpxchg->getPointerOperand();
    return nullptr;
}

// Returns a human-readable location string for an instruction (function:block).
std::string DataRaceChecker::getInstructionLocation(const Instruction* inst) const {
    std::string location;
    raw_string_ostream os(location);
    if (const Function* func = inst->getFunction())
        os << func->getName();
    if (const BasicBlock* bb = inst->getParent())
        os << ":" << bb->getName();
    return os.str();
}

} // namespace concurrency
