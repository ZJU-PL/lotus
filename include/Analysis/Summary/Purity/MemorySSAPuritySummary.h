/** @file MemorySSAPuritySummary.h @brief Memory SSA-based purity summary computation. */
#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"

#include <memory>
#include <optional>

namespace lotus {
namespace analysis {
namespace purity {

struct MemorySSAPuritySummary {
  bool readsReachableMemory = false;
  bool writesReachableMemory = false;
};

class MemorySSAPuritySummaryProvider {
public:
  explicit MemorySSAPuritySummaryProvider(llvm::Module &module);
  ~MemorySSAPuritySummaryProvider();

  bool hasInstrumentedIR() const;

  std::optional<MemorySSAPuritySummary>
  getFunctionSummary(const llvm::Function &function) const;

  std::optional<MemorySSAPuritySummary>
  getCallSummary(const llvm::CallBase &call) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace purity
} // namespace analysis
} // namespace lotus
