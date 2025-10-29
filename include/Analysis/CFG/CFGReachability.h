#ifndef ANALYSIS_CFG_CFGREACHABILITY_H
#define ANALYSIS_CFG_CFGREACHABILITY_H

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

#include <map>

using namespace llvm;

/// CFGReachability - Provides reachability analysis for basic blocks and
/// instructions within a function's control flow graph.
class CFGReachability {
private:
    typedef BitVector ReachableVec;
    ReachableVec AnalyzedVec;
    ReachableVec *ReachableVecPtr;

    /// ID mapping
    std::vector<BasicBlock *> ID2BB;
    std::map<BasicBlock *, int> BB2ID;

public:
    explicit CFGReachability(Function *);

    ~CFGReachability();

    /// Returns true if there is a path from From to To in the CFG.
    bool reachable(BasicBlock *, BasicBlock *);

    /// Returns true if there is a path from From to To instruction.
    bool reachable(Instruction *, Instruction *);

private:
    /// Analyzes reachability to the given basic block using BFS.
    void analyze(BasicBlock *);
};

typedef std::shared_ptr<CFGReachability> CFGReachabilityRef;

#endif // ANALYSIS_CFG_CFGREACHABILITY_H

