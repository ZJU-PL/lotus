#ifndef DATA_RACE_CHECKER_H
#define DATA_RACE_CHECKER_H

#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Analysis/Concurrency/MHPAnalysis.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/InstIterator.h>

#include <unordered_map>
#include <vector>
#include <string>

namespace lotus {
class AliasAnalysisWrapper;
}

namespace concurrency {

/**
 * @brief Specialized checker for data race detection
 *
 * This class handles the logic for detecting data races between concurrent
 * memory accesses that may happen in parallel without proper synchronization.
 */
class DataRaceChecker {
public:
    explicit DataRaceChecker(llvm::Module& module,
                           mhp::MHPAnalysis* mhpAnalysis,
                           lotus::AliasAnalysisWrapper* aliasAnalysis = nullptr);

    /**
     * @brief Check for data races in the module
     * @return Vector of data race reports
     */
    std::vector<ConcurrencyBugReport> checkDataRaces();

private:
    // Analysis components
    llvm::Module& m_module;
    mhp::MHPAnalysis* m_mhpAnalysis;
    lotus::AliasAnalysisWrapper* m_aliasAnalysis;

    // Helper methods for data race detection
    bool mayAlias(const llvm::Value* v1, const llvm::Value* v2) const;
    bool isMemoryAccess(const llvm::Instruction* inst) const;
    bool isWriteAccess(const llvm::Instruction* inst) const;
    bool isAtomicOperation(const llvm::Instruction* inst) const;
    const llvm::Value* getMemoryLocation(const llvm::Instruction* inst) const;
    std::string getInstructionLocation(const llvm::Instruction* inst) const;

    // Data race detection logic
    void collectVariableAccesses(std::unordered_map<const llvm::Value*,
                                 std::vector<const llvm::Instruction*>>& variableAccesses);
    bool mayAccessSameLocation(const llvm::Instruction* inst1,
                              const llvm::Instruction* inst2) const;
};

} // namespace concurrency

#endif // DATA_RACE_CHECKER_H
