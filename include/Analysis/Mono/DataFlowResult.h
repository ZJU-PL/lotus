#ifndef ANALYSIS_DATAFLOWRESULT_H_
#define ANALYSIS_DATAFLOWRESULT_H_

#include "LLVMUtils/SystemHeaders.h"


class DataFlowResult {
public:
  /*
   * Methods
   */
  DataFlowResult();

  std::set<Value *> &GEN(Instruction *inst);
  std::set<Value *> &KILL(Instruction *inst);
  std::set<Value *> &IN(Instruction *inst);
  std::set<Value *> &OUT(Instruction *inst);

private:
  std::map<Instruction *, std::set<Value *>> gens;
  std::map<Instruction *, std::set<Value *>> kills;
  std::map<Instruction *, std::set<Value *>> ins;
  std::map<Instruction *, std::set<Value *>> outs;
};


#endif // ANALYSIS_DATAFLOWRESULT_H_
