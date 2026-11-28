/** @file LinuxKernelChecker.h @brief Dedicated Linux kernel concurrency checker. */
#pragma once

#include "Checker/Concurrency/ConcurrencyBugReport.h"
#include "Concurrency/LinuxKernel/LinuxKernelAnalysis.h"

#include <memory>
#include <vector>

#include <llvm/IR/Module.h>

namespace concurrency {

class LinuxKernelChecker {
public:
  explicit LinuxKernelChecker(llvm::Module &module,
                              kernel::LinuxKernelAnalysis *analysis = nullptr);

  std::vector<ConcurrencyBugReport> checkLinuxKernelBugs();

private:
  llvm::Module &m_module;
  kernel::LinuxKernelAnalysis *m_analysis = nullptr;
  std::unique_ptr<kernel::LinuxKernelAnalysis> m_ownedAnalysis;

  void ensureAnalysis();
};

} // namespace concurrency
