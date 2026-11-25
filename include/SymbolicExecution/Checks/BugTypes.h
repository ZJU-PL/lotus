/** @file BugTypes.h @brief Bug categories, trace steps, and thread-safe report collector. */
#pragma once

#include "SymbolicExecution/Engine/TaintState.h"

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SymbolicExecution {

enum SymexBugType : unsigned {
  BUG_TY_UNDEF = 0x0,
  BUG_TY_BOF = 0x01,
  BUG_TY_DBZ = 0x10,
  BUG_TY_INT_OVERFLOW = 0x100,
  BUG_TY_INT_UNDERFLOW = 0x200,
  BUG_TY_NULL_DEREF = 0x400,
  BUG_TY_SIGNED_INT_OVERFLOW = 0x800,
  BUG_TY_SIGNED_INT_UNDERFLOW = 0x1000,
  BUG_TY_SHIFT_OVERFLOW = 0x2000,
  BUG_TY_ARRAY_INDEX_OOB = 0x4000,
  BUG_TY_UNINIT_READ = 0x8000,
  BUG_TY_UAF = 0x10000,
  BUG_TY_DOUBLE_FREE = 0x20000,
  BUG_TY_NEGATIVE_ARRAY_INDEX = 0x40000,
  BUG_TY_INT_TRUNCATION = 0x80000,
};

struct TraceStep {
  enum TraceStepKind {
    TRACE_STEP_CALL,
    TRACE_STEP_ALLOC,
    TRACE_STEP_BUFFER_ACCESS,
    TRACE_STEP_DIV,
    TRACE_STEP_ARITH,
    TRACE_STEP_DEREF
  };

  TraceStep(TraceStepKind TK, llvm::Instruction *Inst, llvm::Value *Val)
      : TK(TK), Inst(Inst), Val(Val) {}

  TraceStepKind TK;
  llvm::Instruction *Inst;
  llvm::Value *Val;
};

struct BugReportRecord {
  SymexBugType BugTy = BUG_TY_UNDEF;
  std::vector<TaintStep> TaintSteps;
  std::vector<TraceStep> TraceSteps;
};

/// Thread-safe accumulator for confirmed symbolic execution bug reports.
///
/// Deduplicates reports per sink instruction so multiple collapsed paths emit
/// one stable report.
class BugReportCollector {
public:
  void addReport(BugReportRecord Record) {
    if (Record.TraceSteps.empty()) {
      return;
    }
    llvm::Instruction *SinkPos = Record.TraceSteps.front().Inst;
    std::lock_guard<std::mutex> Lock(Mtx);
    if (SinkInsts.insert(SinkPos).second) {
      Reports.emplace_back(std::move(Record));
    }
  }

  std::vector<BugReportRecord> getReports() const {
    std::lock_guard<std::mutex> Lock(Mtx);
    return Reports;
  }

private:
  mutable std::mutex Mtx;
  std::unordered_set<llvm::Instruction *> SinkInsts;
  std::vector<BugReportRecord> Reports;
};

} // namespace SymbolicExecution
