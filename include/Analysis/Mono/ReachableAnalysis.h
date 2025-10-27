
#ifndef ANALYSIS_REACHABLEANALYSIS_H_
#define ANALYSIS_REACHABLEANALYSIS_H_

#include "LLVMUtils/SystemHeaders.h"
#include "Analysis/Mono/DataFlowResult.h"

// Compute forward reachability using backward dataflow analysis.
// This analysis determines which instructions can be executed from each program point.
DataFlowResult *runReachableAnalysis(Function *f);

DataFlowResult *runReachableAnalysis(
    Function *f,
    std::function<bool(Instruction *i)> filter);

#endif // ANALYSIS_REACHABLEANALYSIS_H_

