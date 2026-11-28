/** @file ConditionVariableChecker.h @brief Condition variable misuse checker for concurrent programs. */
#pragma once

#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Concurrency/API/ThreadAPI.h"

#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace concurrency {

class ConcurrencyContext;

/**
 * @brief Specialized checker for condition variable misuse
 */
class ConditionVariableChecker {
public:
  explicit ConditionVariableChecker(llvm::Module &module, ThreadAPI *threadAPI,
                                    mhp::LockSetAnalysis *locksetAnalysis);

  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    m_aliasAnalysis = aa;
  }
  void setContext(ConcurrencyContext *context) { m_context = context; }

  /**
   * @brief Check for condition variable misuse
   * @return Vector of bug reports
   */
  std::vector<ConcurrencyBugReport> checkConditionVariables();

private:
  llvm::Module &m_module;
  ThreadAPI *m_threadAPI;
  mhp::LockSetAnalysis *m_locksetAnalysis;
  lotus::AliasAnalysisWrapper *m_aliasAnalysis = nullptr;
  ConcurrencyContext *m_context = nullptr;

  // Helper methods
  std::string getInstructionLocation(const llvm::Instruction *inst) const;
  const llvm::Value *getMutexForCV(const llvm::Instruction *waitInst) const;
  bool sameSyncObject(const llvm::Value *lhs, const llvm::Value *rhs) const;
  bool isWaitInsideLoop(const llvm::Instruction *waitInst) const;
};

} // namespace concurrency

