
#include "Analysis/Mono/LiveVariablesAnalysis.h"
#include "Analysis/Mono/DataFlowEngine.h"

using namespace llvm;

// SSA register liveness analysis
DataFlowResult *runLiveVariablesAnalysis(Function *f) {
  auto dfa = DataFlowEngine{};
  
  // GEN[n] = { v | v is an SSA value used by n }
  // Simply collect all non-constant operands
  auto computeGEN = [](Instruction *i, DataFlowResult *df) {
    for (auto &op : i->operands()) {
      // Only SSA values (Instructions and Arguments) are tracked
      // Constants are not "live" in the dataflow sense
      if (isa<Instruction>(op) || isa<Argument>(op)) {
        df->GEN(i).insert(op);
      }
    }
  };
  
  // KILL[n] = { n } if n produces a non-void SSA value
  // In SSA form, each instruction that produces a value defines exactly one SSA value: itself
  auto computeKILL = [](Instruction *i, DataFlowResult *df) {
    if (!i->getType()->isVoidTy()) {
      df->KILL(i).insert(i);
    }
  };
  
  // OUT[n] = ⋃ IN[s] for all CFG successors s of n
  auto computeOUT = [](Instruction * /*inst*/,
                       Instruction *succ,
                       std::set<Value *> &OUT,
                       DataFlowResult *df) {
    auto &inS = df->IN(succ);
    OUT.insert(inS.begin(), inS.end());
  };
  
  // IN[n] = (OUT[n] - KILL[n]) ∪ GEN[n]
  auto computeIN = [](Instruction *inst, 
                      std::set<Value *> &IN, 
                      DataFlowResult *df) {
    auto &gen = df->GEN(inst);
    auto &kill = df->KILL(inst);
    auto &out = df->OUT(inst);
    
    // Add everything in OUT that's not killed
    for (auto *v : out) {
      if (kill.find(v) == kill.end()) {
        IN.insert(v);
      }
    }
    
    // Add everything in GEN
    IN.insert(gen.begin(), gen.end());
  };
  
  return dfa.applyBackward(f, computeGEN, computeKILL, computeIN, computeOUT);
}

