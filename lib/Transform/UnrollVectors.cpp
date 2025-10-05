// The `--unroll-vectors` pass eliminates vector `load`, `store`, `phi`,
// `insertelement`, and `extractelement` instructions by expanding them into
// sequences of scalar instructions.  This covers the remaining cases that may
// be left over after running the built-in `--scalarizer` pass.
//
// Note that the pass does not delete the vector operations on its own, but it
// does leave them unused whenever possible, so that `--dce` can eliminate
// them.
#include "llvm/Pass.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Alignment.h"

using namespace llvm;

const int WORD_SIZE = 64;
const size_t TRACE_NUM_ARGS = 8;

/// Downcast `Ty` to `VectorType*`, but only if it's a vector that we know how
/// to unroll.
static VectorType* unrollableVectorType(Type* Ty, Module* M) {
  if (auto VecTy = dyn_cast<VectorType>(Ty)) {
    // LLVM 14 doesn't have isScalable() method
    // if (VecTy->isScalable()) {
    //   return nullptr;
    // }
    const DataLayout& DL = M->getDataLayout();
    if (DL.getTypeSizeInBits(VecTy->getElementType()).getFixedValue() % 8 != 0) {
      return nullptr;
    }
    return VecTy;
  } else {
    return nullptr;
  }
}

namespace {
struct UnrollVectors : public FunctionPass {
  static char ID;
  UnrollVectors() : FunctionPass(ID) {}

  // TODO: handle ConstantVector

  bool runOnFunction(Function &F) override {
    DenseMap<Value*, SmallVector<Value*, 2>> UnrollMap;
    std::vector<PHINode*> UnrolledPHIs;
    Module* M = F.getParent();

    // Unroll vector-typed phi nodes.
    for (BasicBlock& BB : F) {
      for (PHINode& PHI : BB.phis()) {
        if (VectorType* VecTy = unrollableVectorType(PHI.getType(), M)) {
          // In LLVM 14, use getVectorNumElements instead of getNumElements
          unsigned Count = VecTy->getElementCount().getFixedValue();
          Type* ElemTy = VecTy->getElementType();
          SmallVector<Value*, 2> Elems;
          for (unsigned i = 0; i < Count; ++i) {
            PHINode* ElemPHI = PHINode::Create(ElemTy, PHI.getNumIncomingValues(),
                Twine(PHI.getName()).concat(Twine(i)), &BB);
            Elems.push_back(ElemPHI);
          }
          UnrollMap.try_emplace(&PHI, std::move(Elems));
          UnrolledPHIs.push_back(&PHI);
        }
      }
    }

    // Walk all instructions in dominance order.
    DominatorTree Dom(F);
    std::vector<DomTreeNode*> Pending;
    Pending.push_back(Dom.getRootNode());

    while (Pending.size() > 0) {
      DomTreeNode* Node = Pending.back();
      Pending.pop_back();
      BasicBlock* BB = Node->getBlock();
      for (DomTreeNode* Child : *Node) {
        Pending.push_back(Child);
      }

      for (Instruction& I : *BB) {
        if (!handleKnownInst(&I, UnrollMap)) {
          // TODO: Error if this instruction uses any unrolled value.
        }
      }
    }

    // Set incoming values for all vector-typed phi nodes.
    for (PHINode* PHI : UnrolledPHIs) {
      SmallVector<Value*, 2>& Unrolled = UnrollMap[PHI];
      unsigned Count = cast<VectorType>(PHI->getType())->getElementCount().getFixedValue();
      for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
        BasicBlock* IncBB = PHI->getIncomingBlock(i);
        Value* IncVal = PHI->getIncomingValue(i);

        auto it = UnrollMap.find(IncVal);
        if (it == UnrollMap.end()) {
          llvm::errs() << "failed to find incoming value for unrolled phi node:\n";
          llvm::errs() << "  phi node: " << *PHI << "\n";
          llvm::errs() << "  incoming value: " << *IncVal << "\n";
          abort();
        }
        SmallVector<Value*, 2>& IncElems = it->second;

        for (unsigned j = 0; j < Count; ++j) {
          PHINode* ElemPHI = cast<PHINode>(Unrolled[j]);
          ElemPHI->setIncomingBlock(i, IncBB);
          ElemPHI->setIncomingValue(i, IncElems[j]);
        }
      }
    }

    return true;
  }

  bool handleKnownInst(Instruction* I, DenseMap<Value*, SmallVector<Value*, 2>>& UnrollMap) {
    Module* M = I->getModule();

    if (auto InsertElement = dyn_cast<InsertElementInst>(I)) {
      if (!unrollableVectorType(InsertElement->getType(), M)) {
        return false;
      }

      Value* Vector = InsertElement->getOperand(0);
      Value* Elem = InsertElement->getOperand(1);
      Value* IndexVal = InsertElement->getOperand(2);

      unsigned Index;
      if (auto ConstIndexVal = dyn_cast<ConstantInt>(IndexVal)) {
        Index = ConstIndexVal->getZExtValue();
      } else {
        return false;
      }

      auto it = UnrollMap.find(Vector);
      if (it == UnrollMap.end()) {
        return false;
      }
      SmallVector<Value*, 2> Elems = it->second;
      Elems[Index] = Elem;
      UnrollMap.try_emplace(InsertElement, std::move(Elems));
      return true;

    } else if (auto ExtractElement = dyn_cast<ExtractElementInst>(I)) {
      if (!unrollableVectorType(ExtractElement->getVectorOperandType(), M)) {
        return false;
      }

      Value* Vector = ExtractElement->getOperand(0);
      Value* IndexVal = ExtractElement->getOperand(1);

      unsigned Index;
      if (auto ConstIndexVal = dyn_cast<ConstantInt>(IndexVal)) {
        Index = ConstIndexVal->getZExtValue();
      } else {
        return false;
      }

      auto it = UnrollMap.find(Vector);
      if (it == UnrollMap.end()) {
        return false;
      }
      SmallVector<Value*, 2>& Elems = it->second;
      ExtractElement->replaceAllUsesWith(Elems[Index]);
      return true;

    } else if (auto Load = dyn_cast<LoadInst>(I)) {
      VectorType* VecTy = unrollableVectorType(Load->getType(), M);
      if (!VecTy) {
        return false;
      }

      if (!Load->isSimple()) {
        return false;
      }

      unsigned Count = VecTy->getElementCount().getFixedValue();
      Type* ElemTy = VecTy->getElementType();
      unsigned AddrSpace = Load->getPointerAddressSpace();
      Value* Ptr = Load->getPointerOperand();
      Ptr = CastInst::Create(Instruction::BitCast, Ptr,
          PointerType::get(ElemTy, AddrSpace),
          Twine(Ptr->getName(), "elem"), Load);

      // Updated for LLVM 14 API
      MaybeAlign MAlign = Load->getAlign();
      uint64_t ElemSize = M->getDataLayout().getTypeStoreSize(ElemTy);
      Align LoadAlign = MAlign.valueOrOne();
      
      if (LoadAlign.value() > ElemSize) {
        assert(LoadAlign.value() % ElemSize == 0);
        LoadAlign = Align(ElemSize);
      }

      SmallVector<Value*, 2> Elems;
      for (unsigned i = 0; i < Count; ++i) {
        Value* OffsetPtr;
        if (i == 0) {
          OffsetPtr = Ptr;
        } else {
          Value* Idxs[1] = { ConstantInt::get(IntegerType::get(M->getContext(), 32), i) };
          OffsetPtr = GetElementPtrInst::CreateInBounds(ElemTy, Ptr, Idxs,
              Twine(Ptr->getName()).concat(Twine(i)), Load);
        }
        
        // Updated LoadInst constructor for LLVM 14
        LoadInst* ElemLoad = new LoadInst(ElemTy, OffsetPtr,
            Twine(Load->getName()).concat(Twine(i)), 
            false, LoadAlign, Load);
            
        Elems.push_back(ElemLoad);
      }
      UnrollMap.try_emplace(Load, std::move(Elems));

      return true;

    } else if (auto Store = dyn_cast<StoreInst>(I)) {
      // TODO: handle StoreInst
      return false;

    } else {
      return false;
    }
  }
}; // end of struct UnrollVectors
}  // end of anonymous namespace

char UnrollVectors::ID = 0;
static RegisterPass<UnrollVectors> X(
        "unroll-vectors",
        "Unroll vector ops into sequences of scalar ops",
        false /* Only looks at CFG */,
        false /* Analysis Pass */);