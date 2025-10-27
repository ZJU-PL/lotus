
#include "Analysis/Mono/DataFlow.h"


DataFlowAnalysis::DataFlowAnalysis() {
  return;
}

DataFlowResult *DataFlowAnalysis::getFullSets(Function *f) {

  auto df = new DataFlowResult{};
  for (auto &inst : instructions(*f)) {
    auto &inSetOfInst = df->IN(&inst);
    auto &outSetOfInst = df->OUT(&inst);
    for (auto &inst2 : instructions(*f)) {
      inSetOfInst.insert(&inst2);
      outSetOfInst.insert(&inst2);
    }
  }

  return df;
}


