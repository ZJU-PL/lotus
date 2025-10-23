#include "Checker/concurrency/DataRaceChecker.h"

#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

// #include <algorithm>

using namespace llvm;
using namespace mhp;

namespace concurrency {

DataRaceChecker::DataRaceChecker(Module& module,
                               MHPAnalysis* mhpAnalysis,
                               AAResults* aliasAnalysis)
    : m_module(module), m_mhpAnalysis(mhpAnalysis), m_aliasAnalysis(aliasAnalysis) {}

std::vector<ConcurrencyBugReport> DataRaceChecker::checkDataRaces() {
    std::vector<ConcurrencyBugReport> reports;

    // Collect all memory access instructions grouped by variable
    std::unordered_map<const Value*, std::vector<const Instruction*>> variableAccesses;
    collectVariableAccesses(variableAccesses);

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

                    if (hasWrite && hasPotentialDataRace(inst1, inst2)) {
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

    return reports;
}

void DataRaceChecker::collectVariableAccesses(
    std::unordered_map<const Value*, std::vector<const Instruction*>>& variableAccesses) {

    for (Function& func : m_module) {
        if (func.isDeclaration()) continue;

        for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
            const Instruction* inst = &*I;

            if (isMemoryAccess(inst)) {
                // Get the memory location being accessed
                const Value* memLocation = getMemoryLocation(inst);

                if (memLocation) {
                    variableAccesses[memLocation].push_back(inst);
                }
            }
        }
    }
}

bool DataRaceChecker::hasPotentialDataRace(const Instruction* inst1,
                                          const Instruction* inst2) {
    // Check if they access the same memory location (considering aliases)
    return mayAlias(getMemoryLocation(inst1), getMemoryLocation(inst2));
}

bool DataRaceChecker::mayAlias(const Value* v1, const Value* v2) const {
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

const Value* DataRaceChecker::getMemoryLocation(const Instruction* inst) const {
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

std::string DataRaceChecker::getInstructionLocation(const Instruction* inst) const {
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

} // namespace concurrency
