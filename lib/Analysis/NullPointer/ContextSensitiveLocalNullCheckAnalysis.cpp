/*
 *  Author: rainoftime
 *  Date: 2025-04
 *  Description: Context-sensitive local null check analysis
 */


#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/InstIterator.h>
#include "Analysis/NullPointer/ContextSensitiveLocalNullCheckAnalysis.h"
#include "Analysis/NullPointer/AliasAnalysisAdapter.h"
#include "LLVMUtils/API.h"

ContextSensitiveLocalNullCheckAnalysis::ContextSensitiveLocalNullCheckAnalysis(
    ContextSensitiveNullFlowAnalysis *NFA, Function *F, const Context &Ctx) 
    : F(F), Ctx(Ctx), NEA(F), NFA(NFA), DT(*F) {
    
    // init nca
    for (unsigned K = 0; K < F->arg_size(); ++K) {
        auto *Arg = F->getArg(K);
        if (!Arg->getType()->isPointerTy()) continue;
        auto ArgX = NEA.get(Arg);
        
        // Get the first instruction in the function as the instruction point
        Instruction *FirstInst = nullptr;
        if (!F->empty() && !F->front().empty()) {
            FirstInst = &F->front().front();
        }
        
        if ((FirstInst && NFA->notNull(Arg, Ctx)) || 
            (FirstInst && NFA->notNull(ArgX, Ctx))) continue;
            
        unsigned ID = PtrIDMap.size();
        PtrIDMap[ArgX] = ID;
    }
    
    for (auto &B : *F) {
        for (auto &I : B) {
            for (unsigned K = 0; K < I.getNumOperands(); ++K) {
                auto Op = I.getOperand(K);
                if (!Op->getType()->isPointerTy()) continue;
                auto OpX = NEA.get(Op);
                
                if (NFA->notNull(OpX, Ctx) || NFA->notNull(Op, Ctx)) continue;
                
                auto It = PtrIDMap.find(OpX);
                if (It != PtrIDMap.end()) continue;
                unsigned ID = PtrIDMap.size();
                PtrIDMap[NEA.get(OpX)] = ID;
            }
        }
    }

    // Initialize all data structures
    init();

    // Initialize InstNonNullMap
    for (auto &B: *F) for (auto &I: B) InstNonNullMap[&I] = 0;

    // Run the initial labeling
    label();
}

ContextSensitiveLocalNullCheckAnalysis::~ContextSensitiveLocalNullCheckAnalysis() = default;

bool ContextSensitiveLocalNullCheckAnalysis::mayNull(Value *Ptr, Instruction *Inst) {
    // not used as a ptr
    if (!Ptr->getType()->isPointerTy()) return false;

    // must be nonnull
    if (NFA->notNull(Ptr, Ctx) || NFA->notNull(NEA.get(Ptr), Ctx)) return false;

    // ptrs in unreachable blocks are considered nonnull
    bool AllPredUnreachable = true;
    if (Inst == &Inst->getParent()->front()) {
        for (auto PIt = pred_begin(Inst->getParent()), PE = pred_end(Inst->getParent()); PIt != PE; ++PIt) {
            auto *PredTerm = (*PIt)->getTerminator();
            for (unsigned J = 0; J < PredTerm->getNumSuccessors(); ++J) {
                if (PredTerm->getSuccessor(J) != Inst->getParent()) continue;
                if (!UnreachableEdges.count({PredTerm, J})) {
                    AllPredUnreachable = false;
                    break;
                }
            }
            if (!AllPredUnreachable) break;
        }
    } else {
        AllPredUnreachable = UnreachableEdges.count({Inst->getPrevNode(), 0});
    }
    if (AllPredUnreachable) return false;

    // check nca results
    bool IsOperand = false;
    unsigned K = 0;
    for (; K < Inst->getNumOperands(); ++K) {
        if (Ptr == Inst->getOperand(K)) {
            IsOperand = true;
            break;
        }
    }
    assert(IsOperand && "Ptr must be an operand of Inst!");
    if (K >= 32) return true; // more than 32 operands in this weird instruction
    auto It = InstNonNullMap.find(Inst);
    assert(It != InstNonNullMap.end());
    return !(It->second & (1 << K));
}

void ContextSensitiveLocalNullCheckAnalysis::run() {
    // 1. init a map from each instruction to a set of nonnull pointers
    init();

    // 2. fixed-point algorithm for null check analysis
    nca();

    // 3. tag possible null pointers at each instruction
    tag();

    // 4. label unreachable edges
    label();
}

void ContextSensitiveLocalNullCheckAnalysis::init() {
    // Initialize DataflowFacts for all instructions in the function
    for (auto &B : *F) {
        for (auto &I : B) {
            if (I.isTerminator()) {
                for (unsigned K = 0; K < I.getNumSuccessors(); ++K) {
                    DataflowFacts[{&I, K}] = BitVector(PtrIDMap.size());
                }
            } else {
                DataflowFacts[{&I, 0}] = BitVector(PtrIDMap.size());
            }
        }
    }
    
    // Initialize the entry edge
    DataflowFacts[{nullptr, 0}] = BitVector(PtrIDMap.size());
    
    // Make sure InstNonNullMap is initialized for all instructions
    for (auto &B: *F) {
        for (auto &I: B) {
            InstNonNullMap[&I] = 0;
        }
    }
}

void ContextSensitiveLocalNullCheckAnalysis::tag() {
    BitVector ResultOfMerging;
    std::vector<Edge> IncomingEdges;
    for (auto &B : *F) {
        for (auto &I : B) {
            IncomingEdges.clear();
            if (&I == &B.front()) {
                for (auto PIt = pred_begin(&B), PE = pred_end(&B); PIt != PE; ++PIt) {
                    auto Term = (*PIt)->getTerminator();
                    for (unsigned K = 0; K < Term->getNumSuccessors(); ++K) {
                        if (Term->getSuccessor(K) == I.getParent()) {
                            IncomingEdges.emplace_back(Term, K);
                        }
                    }
                }
            } else {
                IncomingEdges.emplace_back(I.getPrevNode(), 0);
            }
            BitVector *NonNulls;
            if (IncomingEdges.empty()) {
                NonNulls = &DataflowFacts.at({nullptr, 0});
            } else if (IncomingEdges.size() == 1) {
                NonNulls = &DataflowFacts.at(IncomingEdges[0]);
            } else {
                merge(IncomingEdges, ResultOfMerging);
                NonNulls = &ResultOfMerging;
            }
            auto &Orig = InstNonNullMap[&I];
            for (auto K = 0; K < I.getNumOperands(); ++K) {
                auto OpK = I.getOperand(K);
                auto It = PtrIDMap.find(NEA.get(OpK));
                if (It == PtrIDMap.end()) continue;
                auto OpKMustNonNull = NonNulls->test(It->second);
                if (OpKMustNonNull) {
                    Orig = Orig | (1 << K);
                    if (isa<ReturnInst>(&I)) {
                        NFA->add(F, Ctx, OpK, nullptr);
                    } else if (auto *CI = dyn_cast<CallInst>(&I)) {
#if defined(LLVM12)
                        if (K < CI->getNumArgOperands()) NFA->add(F, Ctx, CI, K);
#elif defined(LLVM14)
                        // refer to llvm-12/include/llvm/IR/InstrTypes.h:1321
                        if (K < CI->arg_size()) NFA->add(F, Ctx, CI, K);
#else
    #error "Unsupported LLVM version"
#endif
                    } else {
                        // ... omit others
                    }
                }
            }
        }
    }
}

void ContextSensitiveLocalNullCheckAnalysis::merge(std::vector<Edge> &IncomingEdges, BitVector &Result) {
    auto It = IncomingEdges.begin();
    Result = DataflowFacts.at(*It++);
    while (It != IncomingEdges.end()) {
        auto &NextSet = DataflowFacts.at(*It);
        Result &= NextSet;
        It++;
    }
}

void ContextSensitiveLocalNullCheckAnalysis::transfer(Edge E, const BitVector &In, BitVector &Out) {
    // assume In, evaluate E's fact as Out
    Out = In;

    auto Set = [this, &Out](Value *Ptr) {
        size_t PtrID = UINT64_MAX;
        Ptr = NEA.get(Ptr);
        auto It = PtrIDMap.find(Ptr);
        if (It != PtrIDMap.end())
            PtrID = It->second;
        if (PtrID < Out.size()) Out.set(PtrID);
    };

    auto Count = [this, &In](Value *Ptr) -> bool {
        size_t PtrID = UINT64_MAX;
        Ptr = NEA.get(Ptr);
        auto It = PtrIDMap.find(Ptr);
        if (It != PtrIDMap.end())
            PtrID = It->second;
        if (PtrID < In.size())
            return In.test(PtrID);
        else
            return true;
    };

    // analyze each instruction type
    auto *Inst = E.first;
    auto BrNo = E.second;
    
    if (!Inst) return;
    
    switch (Inst->getOpcode()) {
        case Instruction::Load:
        case Instruction::Store:
        case Instruction::GetElementPtr:
            Set(getPointerOperand(Inst));
            break;
        case Instruction::Alloca:
            Set(Inst);
            break;
        case Instruction::AddrSpaceCast:
        case Instruction::BitCast:
            if (Count(Inst->getOperand(0))) Set(Inst);
            break;
        case Instruction::PHI:
        case Instruction::Select:
            if (Inst->getType()->isPointerTy()) {
                bool AllNonNull = true;
                for (unsigned K = 0; K < Inst->getNumOperands(); ++K) {
                    auto Op = Inst->getOperand(K);
                    if (!Op->getType()->isPointerTy()) continue;
                    if (!Count(Op)) {
                        AllNonNull = false;
                        break;
                    }
                }
                if (AllNonNull) Set(Inst);
            }
            break;
        case Instruction::ICmp: {
            auto *ICmp = dyn_cast<ICmpInst>(Inst);
            if (ICmp->isEquality() && BrNo < 2) {
                auto *Op0 = ICmp->getOperand(0);
                auto *Op1 = ICmp->getOperand(1);
                if (Op0->getType()->isPointerTy() && Op1->getType()->isPointerTy()) {
                    auto *ConstNull = dyn_cast<ConstantPointerNull>(Op1);
                    if (!ConstNull) ConstNull = dyn_cast<ConstantPointerNull>(Op0);
                    if (ConstNull) {
                        auto *NonNullOp = Op0 == ConstNull ? Op1 : Op0;
                        auto *Br = dyn_cast<BranchInst>(ICmp->user_back());
                        if (Br && Br->isConditional()) {
                            if ((ICmp->getPredicate() == ICmpInst::ICMP_EQ && BrNo == 1) ||
                                (ICmp->getPredicate() == ICmpInst::ICMP_NE && BrNo == 0)) {
                                Set(NonNullOp);
                            }
                        }
                    }
                }
            }
            break;
        }
        case Instruction::Call: {
            auto *CI = dyn_cast<CallInst>(Inst);
            if (CI->getType()->isPointerTy()) {
                auto *Callee = CI->getCalledFunction();
                if (Callee && API::isMemoryAllocate(CI)) {
                    Set(CI);
                }
            }
            break;
        }
        case Instruction::Br: {
            auto *Br = dyn_cast<BranchInst>(Inst);
            if (Br->isConditional()) {
                auto *Cond = Br->getCondition();
                if (auto *ICmp = dyn_cast<ICmpInst>(Cond)) {
                    auto *Op0 = ICmp->getOperand(0);
                    auto *Op1 = ICmp->getOperand(1);
                    if (Op0->getType()->isPointerTy() && Op1->getType()->isPointerTy()) {
                        auto *ConstNull = dyn_cast<ConstantPointerNull>(Op1);
                        if (!ConstNull) ConstNull = dyn_cast<ConstantPointerNull>(Op0);
                        if (ConstNull) {
                            auto *NonNullOp = Op0 == ConstNull ? Op1 : Op0;
                            if ((ICmp->getPredicate() == ICmpInst::ICMP_EQ && BrNo == 1) ||
                                (ICmp->getPredicate() == ICmpInst::ICMP_NE && BrNo == 0)) {
                                Set(NonNullOp);
                            }
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

void ContextSensitiveLocalNullCheckAnalysis::nca() {
    std::vector<Edge> WorkList;
    for (auto &B : *F) {
        for (auto &I : B) {
            if (I.isTerminator()) {
                for (unsigned K = 0; K < I.getNumSuccessors(); ++K) WorkList.emplace_back(&I, K);
            } else {
                WorkList.emplace_back(&I, 0);
            }
        }
    }
    std::reverse(WorkList.begin(), WorkList.end());

    std::vector<Edge> IncomingEdges;
    BitVector ResultOfMerging;
    BitVector ResultOfTransfer;
    while (!WorkList.empty()) {
        auto Edge = WorkList.back();
        WorkList.pop_back();
        if (UnreachableEdges.count(Edge)) continue;

        auto EdgeInst = Edge.first;
        auto &EdgeFact = DataflowFacts.at(Edge); // this loop iteration re-compute EdgeFact

        // 1. merge
        IncomingEdges.clear();
        if (EdgeInst == &(EdgeInst->getParent()->front())) {
            auto *B = EdgeInst->getParent();
            for (auto PIt = pred_begin(B), PE = pred_end(B); PIt != PE; ++PIt) {
                auto Term = (*PIt)->getTerminator();
                for (unsigned K = 0; K < Term->getNumSuccessors(); ++K) {
                    if (Term->getSuccessor(K) == B) {
                        IncomingEdges.emplace_back(Term, K);
                    }
                }
            }
        } else {
            IncomingEdges.emplace_back(EdgeInst->getPrevNode(), 0);
        }
        BitVector *NonNulls;
        if (IncomingEdges.empty()) {
            NonNulls = &DataflowFacts.at({nullptr, 0});
        } else if (IncomingEdges.size() == 1) {
            NonNulls = &DataflowFacts.at(IncomingEdges[0]);
        } else {
            merge(IncomingEdges, ResultOfMerging);
            NonNulls = &ResultOfMerging;
        }

        // 2. transfer
        ResultOfTransfer.clear();
        transfer(Edge, *NonNulls, ResultOfTransfer);

        // 3. add necessary ones to worklist
        if (EdgeFact != ResultOfTransfer) {
            EdgeFact = ResultOfTransfer;
            if (EdgeInst && EdgeInst->isTerminator()) {
                auto *Succ = EdgeInst->getSuccessor(Edge.second);
                for (auto &I: *Succ) {
                    if (I.isTerminator()) {
                        for (unsigned K = 0; K < I.getNumSuccessors(); ++K) {
                            WorkList.emplace_back(&I, K);
                        }
                    } else {
                        WorkList.emplace_back(&I, 0);
                    }
                }
            } else if (EdgeInst) {
                auto *Next = EdgeInst->getNextNode();
                if (Next) {
                    if (Next->isTerminator()) {
                        for (unsigned K = 0; K < Next->getNumSuccessors(); ++K) {
                            WorkList.emplace_back(Next, K);
                        }
                    } else {
                        WorkList.emplace_back(Next, 0);
                    }
                }
            }
        }
    }
}

void ContextSensitiveLocalNullCheckAnalysis::label() {
    for (auto &B: *F) {
        for (auto &I: B) {
            if (I.isTerminator()) {
                for (unsigned K = 0; K < I.getNumSuccessors(); ++K) {
                    label({&I, K});
                }
            }
        }
    }
}

void ContextSensitiveLocalNullCheckAnalysis::label(Edge E) {
    auto *Inst = E.first;
    auto BrNo = E.second;
    
    // Check if the edge exists in DataflowFacts before accessing it
    if (DataflowFacts.find(E) == DataflowFacts.end()) {
        return; // Skip this edge if it's not in the map
    }
    
    if (auto *Br = dyn_cast<BranchInst>(Inst)) {
        if (Br->isConditional()) {
            auto *Cond = Br->getCondition();
            if (auto *ICmp = dyn_cast<ICmpInst>(Cond)) {
                auto *Op0 = ICmp->getOperand(0);
                auto *Op1 = ICmp->getOperand(1);
                if (Op0->getType()->isPointerTy() && Op1->getType()->isPointerTy()) {
                    auto *ConstNull = dyn_cast<ConstantPointerNull>(Op1);
                    if (!ConstNull) ConstNull = dyn_cast<ConstantPointerNull>(Op0);
                    if (ConstNull) {
                        auto *NonNullOp = Op0 == ConstNull ? Op1 : Op0;
                        auto It = PtrIDMap.find(NEA.get(NonNullOp));
                        if (It != PtrIDMap.end()) {
                            auto PtrID = It->second;
                            auto &EdgeFact = DataflowFacts.at(E);
                            if (EdgeFact.test(PtrID)) {
                                if ((ICmp->getPredicate() == ICmpInst::ICMP_EQ && BrNo == 0) ||
                                    (ICmp->getPredicate() == ICmpInst::ICMP_NE && BrNo == 1)) {
                                    UnreachableEdges.insert(E);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

Value *getPointerOperand(Instruction *I) {
    if (auto *LI = dyn_cast<LoadInst>(I)) return LI->getPointerOperand();
    if (auto *SI = dyn_cast<StoreInst>(I)) return SI->getPointerOperand();
    if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) return GEP->getPointerOperand();
    return nullptr;
} 