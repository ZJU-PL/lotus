#include "SymbolicExecution/Support/AnalysisContext.h"

#include <algorithm>
#include <functional>

namespace SymbolicExecution {

AnalysisContext::AnalysisContext(llvm::Module *M, const llvm::DataLayout *DL,
                                 GuardedValueFlowBuilderPass *Builder)
    : M(M), DL(DL), Builder(Builder) {
  if (M) {
    NonPtrTy = llvm::Type::getInt64Ty(M->getContext());
  }
}

GuardedValueFlowGraph *AnalysisContext::getGraph(llvm::Function *Func) const {
  if (!Builder || !Func || !Builder->hasGraphFor(*Func))
    return nullptr;
  return &Builder->getGraph(*Func);
}

void AnalysisContext::computeCallGraphOrder() {
  FuncSeq.clear();
  FunDepthMap.clear();
  CallInfoTopDown.clear();
  CallInfoBottomUp.clear();

  if (!M)
    return;

  for (llvm::Function &F : *M) {
    CallInfoTopDown[&F];
    CallInfoBottomUp[&F];
  }

  for (llvm::Function &F : *M) {
    for (llvm::BasicBlock &BB : F) {
      for (llvm::Instruction &I : BB) {
        auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
        llvm::Function *Callee = CB ? CB->getCalledFunction() : nullptr;
        if (!Callee)
          continue;
        CallInfoTopDown[&F].insert(Callee);
        CallInfoBottomUp[Callee].insert(&F);
      }
    }
  }

  std::unordered_set<llvm::Function *> Visited;

  std::function<void(llvm::Function *)> DFS = [&](llvm::Function *F) {
    Visited.insert(F);
    for (llvm::Function *Next : CallInfoTopDown[F]) {
      if (!Visited.count(Next))
        DFS(Next);
    }
    FuncSeq.push_back(F);
  };

  for (llvm::Function &F : *M) {
    if (!Visited.count(&F))
      DFS(&F);
  }

  std::reverse(FuncSeq.begin(), FuncSeq.end());

  for (auto It = FuncSeq.rbegin(); It != FuncSeq.rend(); ++It) {
    llvm::Function *F = *It;
    unsigned MaxDepth = 0;
    for (llvm::Function *Callee : CallInfoTopDown[F]) {
      auto DepthIt = FunDepthMap.find(Callee);
      if (DepthIt != FunDepthMap.end())
        MaxDepth = std::max(MaxDepth, DepthIt->second + 1);
    }
    FunDepthMap[F] = MaxDepth;
  }
}

unsigned AnalysisContext::getFunctionDepth(llvm::Function *Func) const {
  auto It = FunDepthMap.find(Func);
  return It == FunDepthMap.end() ? 0u : It->second;
}

bool AnalysisContext::isFunctionTopLevel(llvm::Function *Func) const {
  auto It = CallInfoBottomUp.find(Func);
  return It == CallInfoBottomUp.end() || It->second.empty();
}

uint64_t AnalysisContext::getTypeSizeInBits(llvm::Type *Ty) const {
  if (!DL || !Ty || !Ty->isSized())
    return 0;
  return DL->getTypeSizeInBits(Ty);
}

uint64_t AnalysisContext::getTypeStoreSize(llvm::Type *Ty) const {
  if (!DL || !Ty || !Ty->isSized())
    return 0;
  return DL->getTypeStoreSize(Ty);
}

uint64_t AnalysisContext::getTypeStoreSizeInBits(llvm::Type *Ty) const {
  return getTypeStoreSize(Ty) * 8;
}

uint64_t AnalysisContext::getElementOffset(llvm::StructType *St,
                                           unsigned Idx) const {
  if (!DL || !St || Idx >= St->getNumElements())
    return 0;
  return DL->getStructLayout(St)->getElementOffset(Idx);
}

llvm::Function *getDirectCallee(const llvm::CallBase *CB) {
  if (!CB)
    return nullptr;

  if (llvm::Function *Callee = CB->getCalledFunction())
    return Callee;

  llvm::Value *CalledOperand = CB->getCalledOperand();
  return llvm::dyn_cast_or_null<llvm::Function>(
      CalledOperand ? CalledOperand->stripPointerCasts() : nullptr);
}

llvm::Function *getCallee(llvm::Instruction *I) {
  return getDirectCallee(llvm::dyn_cast_or_null<llvm::CallBase>(I));
}

bool isDefiniteCall(llvm::Instruction *I) {
  llvm::Function *Callee = getCallee(I);
  return Callee && !Callee->isDeclaration();
}

bool isPureLibrary(const llvm::Function *F) {
  return F && (F->doesNotAccessMemory() || F->onlyReadsMemory());
}

bool isMatchLib(llvm::CallInst *CallI, const std::string &CalleeName,
                const std::string &LibName) {
  if (!CallI)
    return false;
  llvm::Function *Callee = CallI->getCalledFunction();
  if (!Callee)
    return false;
  std::string Actual = Callee->getName().str();
  return Actual.find(CalleeName) != std::string::npos &&
         Actual.find(LibName) != std::string::npos;
}

std::vector<std::pair<GuardedValueFlowNode *, GuardedValueFlowRegionNode *>>
getIncomingValuesForLoad(const GuardedValueFlowNode *Node) {
  std::vector<std::pair<GuardedValueFlowNode *, GuardedValueFlowRegionNode *>>
      Result;
  if (!Node)
    return Result;

  for (const auto &Match : Node->getMatchingRegions()) {
    auto *Producer = llvm::dyn_cast_or_null<GuardedValueFlowNode>(Match.producer);
    auto *Region = Match.region;
    if (Producer && Region)
      Result.emplace_back(Producer, Region);
  }
  return Result;
}

} // namespace SymbolicExecution
