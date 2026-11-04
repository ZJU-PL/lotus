#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"

#include "Checker/TypeQualifier/Annotation.h"
#include "Checker/TypeQualifier/Helper.h"
#include "Checker/TypeQualifier/QualifierAnalysis.h"

#include <cstring>
#include <deque>

void AAMapMerge(AAMap &, AAMap &);
void FuncAnalysis::computeAASet() {
  AAMap in;
  AAMap out;
  AAMap old;

  const DataLayout *DL = &(F->getParent()->getDataLayout());
  std::deque<Instruction *> worklist;
  for (Function::iterator BB = F->begin(); BB != F->end(); BB++) {
    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); I++) {
      worklist.push_back(&*I);
    }
  }

  Instruction *entry = &(F->front().front());
  unsigned count = 0;
  unsigned threshold = 20 * worklist.size();
  while (!worklist.empty()) {
    if (count++ > threshold)
      break;
    Instruction *I = worklist.front();
    worklist.pop_front();

    in.clear();
    out.clear();
    old.clear();

    old = nAAMap[I];
    BasicBlock *block = I->getParent();
    Instruction *firstIns = &(block->front());
    Instruction *lastInst = (Instruction *)block->getTerminator();

    if (firstIns == I) {
      if (I == entry) {
        for (NodeIndex i = 0; i < nodeFactory.getNumNodes(); i++) {
          in[i].insert(i);
        }
      } else {

        for (auto pi = pred_begin(block); pi != pred_end(block); pi++) {
          BasicBlock *prev = *pi;
          Instruction *src = (Instruction *)prev->getTerminator();
          AAMapMerge(in, nAAMap[src]);
        }
      }
    } else {
      Instruction *prev = I->getPrevNode();
      in = nAAMap[prev];
    }

    switch (I->getOpcode()) {
    case Instruction::Alloca: {
      out = in;
      if (I->getType()->isPointerTy()) {
        NodeIndex valNode = nodeFactory.getValueNodeFor(I);
        assert(valNode != AndersNodeFactory::InvalidIndex &&
               "Failed to find alloca value node");
        out[valNode].insert(valNode);
      }
      break;
    }
    case Instruction::GetElementPtr: {
      out = in;
      assert(I->getType()->isPointerTy());

      NodeIndex srcIndex = nodeFactory.getValueNodeFor(I->getOperand(0));
      assert(srcIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find gep src node");
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(I);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find gep dst node");
      int64_t offset = getGEPOffset(I, DL);
      unsigned fieldNum = 0;
      bool onlyUnion = true;
      for (auto obj : nPtsGraph[I][dstIndex]) {
        if (obj <= nodeFactory.getConstantIntNode())
          continue;
        if (!nodeFactory.isUnOrArrObjNode(obj)) {
          onlyUnion = false;
        }
      }
      Type *srcTy = I->getOperand(0)->getType()->getPointerElementType();
      if (StructType *srcST = dyn_cast<StructType>(srcTy)) {
        if (!srcST->isLiteral() && srcST->getStructName().startswith("union")) {
          onlyUnion = true;
          break;
        }
      }
      if (!onlyUnion && offset > 0)
        fieldNum = offsetToFieldNum(I->getOperand(0)->stripPointerCasts(),
                                    offset, DL, &Ctx->structAnalyzer, M);

      out[dstIndex].clear();
      out[dstIndex].insert(dstIndex);
      if (fieldNum == 0) {
        out[dstIndex].insert(srcIndex);
      }
      //%1 = getelementptr inbounds %struct.A, %A, 0, 0
      // This means the ptr is the alias of the value node of %A
      // However, if the field number is not at the begining, then no one will
      // be tha aa with it. erase the code, 'cause may not useful if (fieldNum
      // == 0)
      //{
      //    for (auto aa : nAAMap[I][srcIndex])
      //    {
      //        out[dstIndex].insert(aa);
      //    }
      //}

      break;
    }
    case Instruction::BitCast:
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt: {
      out = in;
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(I->getOperand(0));
      assert(srcIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find bitcast src node");
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(I);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find bitcast dst node");

      out[dstIndex].insert(dstIndex);
      for (auto aa : in[srcIndex])
        out[dstIndex].insert(aa);

      break;
    }
    case Instruction::Store: {
      out = in;
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(I->getOperand(0));
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(I->getOperand(1));

      for (auto i : nPtsGraph[I][dstIndex]) {
        out[i].clear();
        out[i].insert(i);
        out[i].insert(srcIndex);
        for (auto aa : in[srcIndex])
          out[i].insert(aa);
      }
      break;
    }
    case Instruction::Load: {
      out = in;

      NodeIndex opIndex = nodeFactory.getValueNodeFor(I->getOperand(0));
      assert(opIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find load operand node");
      NodeIndex valIndex = nodeFactory.getValueNodeFor(I);
      assert(valIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find load value node");
      out[valIndex].insert(valIndex);
      for (auto i : nPtsGraph[I][opIndex]) {
        if (in.find(i) != in.end()) {
          for (auto item : in[i]) {
            out[valIndex].insert(item);
          }
        }
      }
      break;
    }
    case Instruction::PHI: {
      out = in;

      const PHINode *phiInst = cast<PHINode>(I);
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(phiInst);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find phi dst node");
      out[dstIndex].insert(dstIndex);
      for (unsigned int i = 0; i < phiInst->getNumIncomingValues(); i++) {
        if (isa<UndefValue>(phiInst->getIncomingValue(i)))
          continue;
        NodeIndex phiIndex =
            nodeFactory.getValueNodeFor(phiInst->getIncomingValue(i));
        assert(phiIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find phi src node");
        if (in.find(phiIndex) != in.end()) {
          out[dstIndex].insert(phiIndex);
          out[dstIndex].insert(in[phiIndex].begin(), in[phiIndex].end());
        }
      }

      break;
    }
    case Instruction::IntToPtr: {
      out = in;
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(I);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find inttoptr dst node");

      out[dstIndex].insert(dstIndex);

      // We use pattern matching to look for a matching ptrtoint
      Value *op = I->getOperand(0);

      // Pointer copy: Y = inttoptr (ptrtoint X)
      Value *srcValue = nullptr;
      if (PatternMatch::match(
              op, PatternMatch::m_PtrToInt(PatternMatch::m_Value(srcValue)))) {
        NodeIndex srcIndex = nodeFactory.getValueNodeFor(srcValue);
        assert(srcIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find inttoptr src node");
        out[dstIndex].insert(srcIndex);
        if (in.find(srcIndex) != in.end()) {
          for (auto i : in[srcIndex]) {
            out[dstIndex].insert(i);
          }
        }
        break;
      }
      // alias in qualifier sense, not strictly the alias
      //  Pointer arithmetic: Y = inttoptr (ptrtoint (X) + offset)
      if (PatternMatch::match(
              op, PatternMatch::m_Add(
                      PatternMatch::m_PtrToInt(PatternMatch::m_Value(srcValue)),
                      PatternMatch::m_Value()))) {
        NodeIndex srcIndex = nodeFactory.getValueNodeFor(srcValue);
        assert(srcIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find inttoptr src node");
        out[dstIndex].insert(srcIndex);
        if (in.find(srcIndex) != in.end()) {
          for (auto i : in[srcIndex]) {
            out[dstIndex].insert(i);
          }
        }
        break;
      }

      if (!isa<Constant>(op))
        out[dstIndex].insert(nodeFactory.getUniversalPtrNode());

      break;
    }
    case Instruction::Select: {
      out = in;

      NodeIndex dstIndex = nodeFactory.getValueNodeFor(I);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find select dst node");
      out[dstIndex].insert(dstIndex);

      NodeIndex srcIndex1 = nodeFactory.getValueNodeFor(I->getOperand(1));
      assert(srcIndex1 != AndersNodeFactory::InvalidIndex &&
             "Failed to find select src node 1");
      out[dstIndex].insert(srcIndex1);

      NodeIndex srcIndex2 = nodeFactory.getValueNodeFor(I->getOperand(2));
      assert(srcIndex2 != AndersNodeFactory::InvalidIndex &&
             "Failed to find select src node 2");
      out[dstIndex].insert(srcIndex2);

      break;
    }
    default: {
      out = in;
      break;
    }
    } // switch (I -> getOpcode())
    nAAMap[I] = out;

    // push the succ of changed node
    if (nAAMap[I] != old) {
      Instruction *next;
      if (lastInst == I) {
        for (auto si = succ_begin(block); si != succ_end(block); si++) {
          BasicBlock *succ = *si;
          next = &(succ->front());
          if (std::find(worklist.begin(), worklist.end(), next) ==
              worklist.end())
            worklist.push_back(next);
        }
      } else {
        next = I->getNextNode();
        if (std::find(worklist.begin(), worklist.end(), next) ==
            std::end(worklist))
          worklist.push_back(next);
      }
    }

  } // while(worklist.empty())
}
void AAMapMerge(AAMap &s1, AAMap &s2) {
  for (AAMap::iterator i = s2.begin(); i != s2.end(); i++) {
    if (s1.find(i->first) != s1.end()) {
      s1[i->first].insert(i->second.begin(), i->second.end());
    } else {
      s1[i->first] = i->second;
    }
  }
}
