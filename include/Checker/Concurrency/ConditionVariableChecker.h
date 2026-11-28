/** @file ConditionVariableChecker.h @brief Condition variable misuse checker for concurrent programs. */
#pragma once

#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Concurrency/API/ThreadAPI.h"

#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace concurrency {

/**
 * @brief Specialized checker for condition variable misuse
 */
class ConditionVariableChecker {
public:
  explicit ConditionVariableChecker(llvm::Module &module, ThreadAPI *threadAPI,
                                    mhp::LockSetAnalysis *locksetAnalysis);

  /**
   * @brief Check for condition variable misuse
   * @return Vector of bug reports
   */
  std::vector<ConcurrencyBugReport> checkConditionVariables();

private:
  llvm::Module &m_module;
  ThreadAPI *m_threadAPI;
  mhp::LockSetAnalysis *m_locksetAnalysis;

  // Helper methods
  std::string getInstructionLocation(const llvm::Instruction *inst) const;
  const llvm::Value *getMutexForCV(const llvm::Instruction *waitInst) const;
};

} // namespace concurrency

