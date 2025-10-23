#ifndef CONCURRENCY_BUG_REPORT_H
#define CONCURRENCY_BUG_REPORT_H

#include "Checker/Report/BugTypes.h"
#include <llvm/IR/Instructions.h>
#include <string>

namespace concurrency {

enum class ConcurrencyBugType {
    DATA_RACE,
    DEADLOCK,
    ATOMICITY_VIOLATION
};

struct ConcurrencyBugReport {
    ConcurrencyBugType bugType;
    const llvm::Instruction* instruction1;
    const llvm::Instruction* instruction2;
    std::string description;
    std::string location;
    BugDescription::BugImportance importance;
    BugDescription::BugClassification classification;

    ConcurrencyBugReport(ConcurrencyBugType type,
                        const llvm::Instruction* inst1,
                        const llvm::Instruction* inst2,
                        const std::string& desc,
                        BugDescription::BugImportance imp = BugDescription::BI_HIGH,
                        BugDescription::BugClassification cls = BugDescription::BC_ERROR)
        : bugType(type), instruction1(inst1), instruction2(inst2),
          description(desc), importance(imp), classification(cls) {}
};

} // namespace concurrency

#endif // CONCURRENCY_BUG_REPORT_H
