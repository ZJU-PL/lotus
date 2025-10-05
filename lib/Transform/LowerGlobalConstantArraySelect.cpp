#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
//#include <set>
#include "Transform/LowerGlobalConstantArraySelect.h"

#define DEBUG_TYPE "LowerGlobalConstantArraySelect"

static cl::opt<unsigned> MaxConstantGlobalArray(
        "popeye-lcga-max",
        cl::desc("set the max number of elements of a global array that we try to handle"),
        cl::init(15));

char LowerGlobalConstantArraySelect::ID = 0;
static RegisterPass<LowerGlobalConstantArraySelect> X(DEBUG_TYPE, "G[a] -> switch(a) -> phi(G[x])");

void LowerGlobalConstantArraySelect::getAnalysisUsage(AnalysisUsage &AU) const {
}

bool LowerGlobalConstantArraySelect::runOnModule(Module &M) {
    std::vector<Instruction *> ToRemoveLd;
    std::vector<Instruction *> ToRemoveGep;
    for (auto &F: M) {
        for (auto &B: F) {
            for (auto &I: B) {
                if (!isSelectGlobalConstantArray(I)) continue;
                auto *GP = dyn_cast<GlobalVariable>(I.getOperand(0));
                auto Idx = I.getOperand(2);
                auto ConstArray = dyn_cast<ConstantDataArray>(GP->getInitializer());
                auto *ElmtTy = ConstArray->getElementType();
                auto *NextLoad = I.getNextNode();

                Function *Func = nullptr;
                auto It = SelectFuncMap.find(GP);
                if (It != SelectFuncMap.end()) {
                    Func = It->second;
                }
                if (!Func) {
                    std::vector<Type *> ArgTyVec;
                    ArgTyVec.push_back(Idx->getType());
                    auto *FuncTy = FunctionType::get(ElmtTy, ArgTyVec, false);
                    Func = Function::Create(FuncTy, GlobalValue::InternalLinkage, "select", M);
                    SelectFuncMap[GP] = Func;
                    initialize(Func, ConstArray);
                }
                std::vector<Value *> ArgVec;
                ArgVec.push_back(Idx);

                auto *Call = CallInst::Create(Func->getFunctionType(), Func, ArgVec, "", &I);
                for (auto UserIt = I.user_begin(), E = I.user_end(); UserIt != E; ++UserIt) {
                    auto *User = *UserIt;
                    if (auto *LD = dyn_cast<LoadInst>(User)) {
                        User->replaceAllUsesWith(Call);
                        ToRemoveLd.push_back(LD);
                    }
                }
                ToRemoveGep.push_back(&I);
            }
        }
    }

    for (auto *I: ToRemoveLd)
        I->eraseFromParent();
    for (auto *I: ToRemoveGep) if (I->user_empty()) I->eraseFromParent();

    if (verifyModule(M, &errs()))
        llvm_unreachable("Error: LowerGlobalConstantArraySelect fails...");

    return false;
}

bool LowerGlobalConstantArraySelect::isSelectGlobalConstantArray(Instruction &I) {
    auto *GEP = dyn_cast<GetElementPtrInst>(&I);
    if (!GEP) return false;

    auto *Pointer = GEP->getPointerOperand();
    auto *GVPtr = dyn_cast<GlobalVariable>(Pointer);
    if (!GVPtr || !GVPtr->hasInitializer()) return false;

    auto Initializer = dyn_cast_or_null<ConstantDataArray>(GVPtr->getInitializer());
    if (!Initializer || Initializer->getNumElements() > MaxConstantGlobalArray.getValue()) return false;
    if (!Initializer->getElementType()->isIntegerTy()) return false;

    if (GEP->getNumIndices() != 2) return false;
    auto FirstIndex = dyn_cast_or_null<ConstantInt>(GEP->getOperand(1));
    if (!FirstIndex || FirstIndex->getZExtValue() != 0) return false;
    auto SecondIndex = GEP->getOperand(2);
    if (isa<ConstantInt>(SecondIndex)) return false;

    auto NextInst = dyn_cast_or_null<LoadInst>(I.getNextNode());
    if (!NextInst) return false;

    return NextInst->getPointerOperand() == GEP;
}

void LowerGlobalConstantArraySelect::initialize(Function *F, ConstantDataArray *CDA) {
    auto &Ctx = F->getContext();
    auto *Entry = BasicBlock::Create(Ctx, "", F, nullptr);
    auto *Idx = F->getArg(0);

    auto *Default = BasicBlock::Create(Ctx, "default", F, nullptr);
    new UnreachableInst(Ctx, Default);
    std::vector<std::pair<BasicBlock *, APInt>> Cases;
    for (unsigned K = 0; K < CDA->getNumElements(); ++K) {
        Cases.emplace_back(BasicBlock::Create(Ctx, "", F, nullptr),
                           CDA->getElementAsAPInt(K));
    }

    auto SI = SwitchInst::Create(Idx, Default, Cases.size(), Entry);
    unsigned CaseIdx = 0;
    for (auto &P: Cases) {
        SI->addCase(ConstantInt::get(cast<IntegerType>(Idx->getType()), CaseIdx++), P.first);
    }

    auto RetBlock = BasicBlock::Create(Ctx, "ret", F, nullptr);
    for (auto &P: Cases) {
        BranchInst::Create(RetBlock, P.first);
    }
    auto *RetVal = PHINode::Create(F->getReturnType(), Cases.size(), "", RetBlock);
    for (auto &P: Cases) {
        RetVal->addIncoming(ConstantInt::get(Ctx, P.second), P.first);
    }
    ReturnInst::Create(Ctx, RetVal, RetBlock);
}
