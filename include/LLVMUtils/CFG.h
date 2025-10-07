#ifndef SUPPORT_CFG_H
#define SUPPORT_CFG_H

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

#include <map>

using namespace llvm;

class CFG {
private:
    typedef BitVector ReachableVec;
    ReachableVec AnalyzedVec;
    ReachableVec *ReachableVecPtr;

    /// ID mapping
    std::vector<BasicBlock *> ID2BB;
    std::map<BasicBlock *, int> BB2ID;

public:
    explicit CFG(Function *);

    ~CFG();

    bool reachable(BasicBlock *, BasicBlock *);

    bool reachable(Instruction *, Instruction *);

private:
    void analyze(BasicBlock *);
};

typedef std::shared_ptr<CFG> CFGRef;

#endif //SUPPORT_CFG_H
