/** @file Options.h @brief Configuration options for KINT integer analysis. */
#pragma once

#include <llvm/Support/CommandLine.h>

namespace kint {

// Define a category for performance options
extern llvm::cl::OptionCategory PerformanceCategory;

// Add a timeout option
extern llvm::cl::opt<unsigned> FunctionTimeout;
// Limit on path exploration per function (0 = no limit)
extern llvm::cl::opt<unsigned> MaxPathsPerFunction;
extern llvm::cl::opt<unsigned> SummaryTimeout;
extern llvm::cl::opt<unsigned> SummaryMaxPathsPerFunction;
// Maximum number of times each loop backedge may be traversed along a single
// path (under-approximating loop unrolling in Incorrectness Logic style).
extern llvm::cl::opt<unsigned> MaxLoopUnroll;
// Analyze every function initialized by range analysis instead of only entry
// points selected by taint/main discovery.
extern llvm::cl::opt<bool> AnalyzeAllFunctions;
enum class SummaryMode {
  Off,
  On,
  Required,
};
extern llvm::cl::opt<SummaryMode> InterprocSummaryMode;

// Define a category for checker options
extern llvm::cl::OptionCategory CheckerCategory;

// Check selection is configured by the unified checker frontend.
extern bool CheckIntOverflow;
extern bool CheckDivByZero;
extern bool CheckBadShift;
extern bool CheckArrayOOB;
extern bool CheckDeadBranch;
extern llvm::cl::opt<bool> RobustReachability;
extern llvm::cl::opt<std::string> DumpEFConstraints;
extern llvm::cl::opt<bool> RobustUniversalUnknownLoads;
extern llvm::cl::opt<bool> RobustUniversalExternalGlobals;
extern llvm::cl::opt<bool> RobustUniversalInlineAsm;
extern llvm::cl::opt<std::string> RobustChecks;

// Initialize command line options
void initializeCommandLineOptions();

} // namespace kint
