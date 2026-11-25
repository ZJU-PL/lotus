/** @file SymbolicExecutionWrapper.h @brief LLVM pass wrapper for symbolic execution analysis. */
#pragma once

#include "SymbolicExecution/Checks/BugTypes.h"

#include "llvm/Pass.h"

namespace SymbolicExecution {

/// LLVM module pass entry point for the SymbolicExecution subsystem.
///
/// Wires together AnalysisContext, TaintModel, AnalysisDriver, and QueryEngine,
/// then translates collected BugReportRecords into Lotus BugReportMgr reports.
class SymbolicExecutionWrapper : public llvm::ModulePass {
public:
  static char ID;

  explicit SymbolicExecutionWrapper(unsigned EnabledBugTypes = BUG_TY_BOF);
  ~SymbolicExecutionWrapper() override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnModule(llvm::Module &M) override;

private:
  unsigned EnabledBugTypes = BUG_TY_BOF;

  static int getBugTypeId(SymexBugType BugType);
  void emitBugReports(const BugReportCollector &Collector) const;
};

} // namespace SymbolicExecution
