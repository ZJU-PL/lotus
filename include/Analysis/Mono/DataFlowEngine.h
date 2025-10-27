
#ifndef ANALYSIS_DATAFLOWENGINE_H_
#define ANALYSIS_DATAFLOWENGINE_H_

#include "LLVMUtils/SystemHeaders.h"
#include "Analysis/Mono/DataFlowResult.h"

// Forward declarations for optional memory analysis support
namespace llvm {
  class AAResults;
  class MemorySSA;
}

class DataFlowEngine {
public:
  /*
   * Constructors
   */
  DataFlowEngine();
  
  // Constructor with optional alias analysis and memory SSA support
  DataFlowEngine(AAResults *AA, MemorySSA *MSSA = nullptr);
  
  /*
   * Accessors for optional analysis results
   */
  AAResults *getAliasAnalysis() const { return AA; }
  MemorySSA *getMemorySSA() const { return MSSA; }
  bool hasAliasAnalysis() const { return AA != nullptr; }
  bool hasMemorySSA() const { return MSSA != nullptr; }

  DataFlowResult *applyForward(
      Function *f,
      std::function<void(Instruction *, DataFlowResult *)> computeGEN,
      std::function<void(Instruction *, DataFlowResult *)> computeKILL,
      std::function<void(Instruction *inst, std::set<Value *> &IN)>
          initializeIN,
      std::function<void(Instruction *inst, std::set<Value *> &OUT)>
          initializeOUT,
      std::function<void(Instruction *inst,
                         Instruction *predecessor,
                         std::set<Value *> &IN,
                         DataFlowResult *df)> computeIN,
      std::function<void(Instruction *inst,
                         std::set<Value *> &OUT,
                         DataFlowResult *df)> computeOUT);

  DataFlowResult *applyForward(
      Function *f,
      std::function<void(Instruction *, DataFlowResult *)> computeGEN,
      std::function<void(Instruction *inst, std::set<Value *> &IN)>
          initializeIN,
      std::function<void(Instruction *inst, std::set<Value *> &OUT)>
          initializeOUT,
      std::function<void(Instruction *inst,
                         Instruction *predecessor,
                         std::set<Value *> &IN,
                         DataFlowResult *df)> computeIN,
      std::function<void(Instruction *inst,
                         std::set<Value *> &OUT,
                         DataFlowResult *df)> computeOUT);

  DataFlowResult *applyBackward(
      Function *f,
      std::function<void(Instruction *, DataFlowResult *)> computeGEN,
      std::function<void(Instruction *, DataFlowResult *)> computeKILL,
      std::function<void(Instruction *inst,
                         std::set<Value *> &IN,
                         DataFlowResult *df)> computeIN,
      std::function<void(Instruction *inst,
                         Instruction *successor,
                         std::set<Value *> &OUT,
                         DataFlowResult *df)> computeOUT);

  DataFlowResult *applyBackward(
      Function *f,
      std::function<void(Instruction *, DataFlowResult *)> computeGEN,
      std::function<void(Instruction *inst,
                         std::set<Value *> &IN,
                         DataFlowResult *df)> computeIN,
      std::function<void(Instruction *inst,
                         Instruction *successor,
                         std::set<Value *> &OUT,
                         DataFlowResult *df)> computeOUT);

protected:
  void computeGENAndKILL(
      Function *f,
      std::function<void(Instruction *, DataFlowResult *)> computeGEN,
      std::function<void(Instruction *, DataFlowResult *)> computeKILL,
      DataFlowResult *df);

private:
  DataFlowResult *applyGeneralizedForwardAnalysis(
      Function *f,
      std::function<void(Instruction *, DataFlowResult *)> computeGEN,
      std::function<void(Instruction *, DataFlowResult *)> computeKILL,
      std::function<void(Instruction *inst, std::set<Value *> &IN)>
          initializeIN,
      std::function<void(Instruction *inst, std::set<Value *> &OUT)>
          initializeOUT,
      std::function<std::list<BasicBlock *>(BasicBlock *bb)> getPredecessors,
      std::function<std::list<BasicBlock *>(BasicBlock *bb)> getSuccessors,
      std::function<void(Instruction *inst,
                         Instruction *predecessor,
                         std::set<Value *> &IN,
                         DataFlowResult *df)> computeIN,
      std::function<void(Instruction *inst,
                         std::set<Value *> &OUT,
                         DataFlowResult *df)> computeOUT,
      std::function<void(std::list<BasicBlock *> &workingList, BasicBlock *bb)>
          appendBB,
      std::function<Instruction *(BasicBlock *bb)> getFirstInstruction,
      std::function<Instruction *(BasicBlock *bb)> getLastInstruction,
      std::function<std::set<Value *> &(DataFlowResult *df,
                                        Instruction *instruction)>
          getInSetOfInst,
      std::function<std::set<Value *> &(DataFlowResult *df,
                                        Instruction *instruction)>
          getOutSetOfInst,
      std::function<BasicBlock::iterator(BasicBlock *)> getEndIterator,
      std::function<void(BasicBlock::iterator &)> incrementIterator);

  /*
   * Optional analysis results for memory-aware analyses
   */
  AAResults *AA;
  MemorySSA *MSSA;
};

#endif // ANALYSIS_DATAFLOWENGINE_H_
