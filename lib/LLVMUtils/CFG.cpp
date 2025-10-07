#include "LLVMUtils/CFG.h"
#include <llvm/IR/CFG.h>

// Constructs a CFG analyzer for the given function.
CFG::CFG(Function *F) : AnalyzedVec(F->size(), false) {
  ReachableVecPtr = new ReachableVec[F->size()];
  int Idx = 0;
  for (auto &B : *F) {
    ID2BB.push_back(&B);
    BB2ID[&B] = Idx;
    ReachableVecPtr[Idx].resize(F->size(), false);
    ++Idx;
  }
}

// Destructor cleans up the reachability vectors.
CFG::~CFG() { delete[] ReachableVecPtr; }

// Returns true if there is a path from From to To in the CFG.
bool CFG::reachable(BasicBlock *From, BasicBlock *To) {
  assert(From && To);
  if (From == To)
    return true;

  assert(BB2ID.count(To) && BB2ID.count(From));
  const unsigned DstBlockID = BB2ID.at(To);
  // If we haven't AnalyzedVect the destination node, run the analysis now
  if (!AnalyzedVec[DstBlockID]) {
    analyze(To);
    AnalyzedVec[DstBlockID] = true;
  }

  // return the result from the cache
  return ReachableVecPtr[DstBlockID][BB2ID.at(From)];
}

// Returns true if there is a path from From to To instruction.
bool CFG::reachable(Instruction *From, Instruction *To) {
  assert(From && To);
  if (From == To)
    return true;

  auto *FromB = From->getParent();
  auto *ToB = To->getParent();
  if (FromB == ToB) {
    while (From) {
      From = From->getNextNode();
      if (From == To)
        return true;
    }
    return false;
  } else {
    return reachable(FromB, ToB);
  }
}

// Analyzes reachability to the given basic block using BFS.
void CFG::analyze(BasicBlock *ToBB) {
  BitVector VisitedVec(AnalyzedVec.size());
  ReachableVec &ToReachability = ReachableVecPtr[BB2ID[ToBB]];
  std::vector<BasicBlock *> Worklist;
  Worklist.push_back(ToBB);
  bool FirstRun = true;
  while (!Worklist.empty()) {
    auto *BB = Worklist.back();
    Worklist.pop_back();
    unsigned BBID = BB2ID[BB];
    if (VisitedVec[BBID])
      continue;
    VisitedVec[BBID] = true;
    if (!FirstRun)
      ToReachability[BBID] = true;
    else
      FirstRun = false;
    for (auto *Pred : predecessors(BB))
      Worklist.push_back(Pred);
  }
}