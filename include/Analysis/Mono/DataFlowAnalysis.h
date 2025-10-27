
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

  DataFlowResult *getFullSets(Function *f);
};

#endif // ANALYSIS_DATAFLOWANALYSIS_H_
