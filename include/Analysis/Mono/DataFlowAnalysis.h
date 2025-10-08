
#ifndef ANALYSIS_DATAFLOWANALYSIS_H_
#define ANALYSIS_DATAFLOWANALYSIS_H_

#include "LLVMUtils/SystemHeaders.h"
#include "Analysis/Mono/DataFlowResult.h"

class DataFlowAnalysis {
public:
  /*
   * Methods
   */
  DataFlowAnalysis();

  DataFlowResult *runReachableAnalysis(Function *f);

  DataFlowResult *runReachableAnalysis(
      Function *f,
      std::function<bool(Instruction *i)> filter);

  DataFlowResult *getFullSets(Function *f);
};

#endif // ANALYSIS_DATAFLOWANALYSIS_H_
