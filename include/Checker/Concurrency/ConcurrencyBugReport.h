#ifndef CONCURRENCY_BUG_REPORT_H
#define CONCURRENCY_BUG_REPORT_H

#include "Checker/Report/BugTypes.h"
#include <llvm/IR/Instructions.h>
#include <string>
#include <vector>

namespace concurrency {

enum class ConcurrencyBugType {
    DATA_RACE,
    DEADLOCK,
    ATOMICITY_VIOLATION,
    LOCK_MISMATCH,
    COND_VAR_MISUSE
};

struct ConcurrencyBugStep {
    const llvm::Instruction* instruction;
    std::string description;
    
    ConcurrencyBugStep(const llvm::Instruction* inst, const std::string& desc)
        : instruction(inst), description(desc) {}
};

struct ConcurrencyBugReport {
    ConcurrencyBugType bugType;
    std::vector<ConcurrencyBugStep> steps;
    std::string description; // High-level description
    BugDescription::BugImportance importance;
    BugDescription::BugClassification classification;

    ConcurrencyBugReport(ConcurrencyBugType type,
                        const std::string& desc,
                        BugDescription::BugImportance imp = BugDescription::BI_HIGH,
                        BugDescription::BugClassification cls = BugDescription::BC_ERROR)
        : bugType(type), description(desc), importance(imp), classification(cls) {}
        
    void addStep(const llvm::Instruction* inst, const std::string& desc) {
        steps.emplace_back(inst, desc);
    }
};

} // namespace concurrency

#endif // CONCURRENCY_BUG_REPORT_H
