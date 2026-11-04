#include "Checker/TypeQualifier/Annotation.h"
#include "Checker/TypeQualifier/QualifierAnalysis.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InstIterator.h>

using namespace llvm;

void FuncAnalysis::createNodeForPointerVal(const Value *V, const Type *T,
                                           const NodeIndex valNode,
                                           PtsGraph &ptsGraph) {
  if (!T->isPointerTy())
    return;
  // OP<<"Inside createNodeForPointerVal:"<<*V<<"\n";
  assert(valNode != AndersNodeFactory::InvalidIndex);

  const Type *type = T->getPointerElementType();
  // OP<<"type = "<<*type<<"\n";
  //  An array is considered a single variable of its type.
  while (const ArrayType *arrayType = dyn_cast<ArrayType>(type))
    type = arrayType->getElementType();

  // Now construct the pointer and memory object variable
  // It depends on whether the type of this variable is a struct or not
  if (const StructType *structType = dyn_cast<StructType>(type)) {
    if (structType->isOpaque()) {
      NodeIndex obj = nodeFactory.createObjectNode(V);
      ptsGraph[valNode].insert(obj);
      // XXX: conservative assumption
      if (type->isPointerTy())
        ptsGraph[obj].insert(nodeFactory.getUniversalPtrNode());
    } else if (!structType->isLiteral() &&
               structType->getStructName().startswith("union")) {
      NodeIndex obj = nodeFactory.createObjectNode(V);
      ptsGraph[valNode].insert(obj);
      nodeFactory.setUnOrArrObjNode(obj);
      // XXX: conservative assumption
      if (type->isPointerTy()) {
        ptsGraph[obj].insert(nodeFactory.getUniversalPtrNode());
      }
    } else {
      processStruct(V, structType, valNode, ptsGraph);
    }

  } else {
    NodeIndex obj = nodeFactory.createObjectNode(V);
    ptsGraph[valNode].insert(obj);

    // XXX: conservative assumption
    if (type->isPointerTy()) {
      ptsGraph[obj].insert(nodeFactory.getUniversalPtrNode());
    }
  }
}

void FuncAnalysis::createInitNodes(PtsGraph &ptsGraph) {

  NodeIndex valNode;
  Type *type;

  assert(!F->isDeclaration() && !F->isIntrinsic());

  ptsGraph[nodeFactory.getUniversalPtrNode()].insert(
      nodeFactory.getUniversalPtrNode());
  ptsGraph[nodeFactory.getNullPtrNode()].insert(nodeFactory.getNullPtrNode());
  for (auto const &gv : M->globals()) {
    const GlobalValue *globalVal = &gv;
    if (globalVal->isDeclaration()) {
      auto itr = Ctx->Gobjs.find(globalVal->getName().str());
      if (itr == Ctx->Gobjs.end())
        continue;
      globalVal = itr->second;
    }
    NodeIndex valNode = nodeFactory.createValueNode(globalVal);
    type = globalVal->getType();
    if (type->isPointerTy())
      createNodeForPointerVal(globalVal, type, valNode, ptsGraph);
  }

  for (auto const &f : *M) {
    if (f.hasAddressTaken()) {
      NodeIndex fVal = nodeFactory.createValueNode(&f);
      // NodeIndex fObj = nodeFactory.createObjectNode(&f);
      // ptsGraph[fVal].insert(fObj);
    }
  }

  // Create return node
  fSummary.sumNodeFactory.createValueNode(F);
  type = F->getReturnType();
  if (!type->isVoidTy()) {
    valNode = nodeFactory.createReturnNode(F);
    if (type->isPointerTy())
      createNodeForPointerVal(F, type, valNode, ptsGraph);
    else {
    }
  }
  // fSummary.sumNodeFactory.createReturnNode(F);

  // Create vararg node
  if (F->getFunctionType()->isVarArg())
    nodeFactory.createVarargNode(F);

  // Add nodes for all formal arguments.
  for (auto const &a : F->args()) {
    const Argument *arg = &a;
    valNode = nodeFactory.createValueNode(arg);
    nodeFactory.setArgNode(valNode);
    fSummary.sumNodeFactory.createValueNode(arg);

    type = arg->getType();
    if (type->isPointerTy()) {
      createNodeForPointerVal(arg, type, valNode, ptsGraph);
      for (auto obj : ptsGraph[valNode]) {
        unsigned objSize = nodeFactory.getObjectSize(obj);
        for (unsigned i = 0; i < objSize; i++) {
          nodeFactory.setArgNode(obj + i);
        }
      }
    }
  }
}

NodeIndex FuncAnalysis::processStruct(const Value *v, const StructType *stType,
                                      const NodeIndex ptr, PtsGraph &ptsGraph) {
  // FIXME: Hope opaque type does not happen in kcfi. We linked whole kernel
  // chunks into the single IR file, and thus there shouldn't be any forward
  // declaration. If there's still, I don't think we can handle this case
  // anyway? :( For now, simply turned off the assertion to test.
#if 0
    // We cannot handle opaque type
    if (stType->isOpaque()) {
        errs() << "Opaque struct type ";
        stType->print(errs());
        errs() << "\n";
        return;
    }
    // assert(!stType->isOpaque() && "Opaque type not supported");
#endif

  // Sanity check
  assert(stType != NULL && "structType is NULL");

  const StructInfo *stInfo = Ctx->structAnalyzer.getStructInfo(stType, M);
  assert(stInfo != NULL && "structInfoMap should have info for all structs!");

  // Empty struct has only one pointer that points to nothing
  if (stInfo->isEmpty()) {
    ptsGraph[ptr].insert(nodeFactory.getNullObjectNode());
    return nodeFactory.getNullObjectNode();
  }

  // Non-empty structs: create one pointer and one target for each field
  uint64_t stSize = stInfo->getExpandedSize();
  // We construct a target variable for each field
  // A better approach is to collect all constant GEP instructions and construct
  // variables only if they are used. We want to do the simplest thing first
  NodeIndex obj = nodeFactory.getObjectNodeFor(v);

  if (obj == AndersNodeFactory::InvalidIndex) {
    obj = nodeFactory.createObjectNode(v);
    if (stInfo->isFieldPointer(0))
      ptsGraph[obj].insert(nodeFactory.getUniversalPtrNode());
    if (stInfo->isFieldUnion(0)) {
      nodeFactory.setUnOrArrObjNode(obj);
      ptsGraph[obj].insert(nodeFactory.getUniversalPtrNode());
    }
    for (unsigned i = 1; i < stSize; ++i) {
      NodeIndex structObj = nodeFactory.createObjectNode(obj, i);
      // pointer field points to univeral
      if (stInfo->isFieldPointer(i)) {
        ptsGraph[structObj].insert(nodeFactory.getUniversalPtrNode());
      }

      if (stInfo->isFieldUnion(i)) {
        nodeFactory.setUnOrArrObjNode(structObj);
        ptsGraph[structObj].insert(nodeFactory.getUniversalPtrNode());
      }
    }
  }
  ptsGraph[ptr].insert(obj);

  return obj;
}
bool FuncAnalysis::warningReported(llvm::Instruction *I, NodeIndex idx) {
#ifdef GROUP_BY_NAME
  if (idx > nodeFactory.getConstantIntNode()) {
    std::string varName = nodeFactory.getNodeName(idx);
    if (warningVars.count(varName)) {
      return true;
    } else {
      warningVars.insert(varName);
      return false;
    }
  }
  return false;
#else

  for (auto aa : nAAMap[I][idx]) {
    if (aa <= nodeFactory.getConstantIntNode())
      continue;
    if (warningSet.find(aa) != warningSet.end()) {
      return true;
    }
    for (auto item : nPtsGraph[I]) {
      // if (warningSet.find(item.first) == warningSet.end())
      //	continue;
      // OP<<"item = "<<item.first<<"\n";
      if (nodeFactory.isObjectNode(aa)) {
        unsigned offset = nodeFactory.getObjectOffset(aa);
        if (warningSet.find(aa - offset) != warningSet.end()) {
          return true;
        }
      } else {
        // OP<<"value node.\n";
        if (warningSet.find(aa) != warningSet.end()) {
          return true;
        }
      }
    }
  }
  for (auto aa : nAAMap[I][idx]) {
    warningSet.insert(aa);
  }
  for (auto item : nPtsGraph[I][idx]) {
    warningSet.insert(item);
  }
  return false;
#endif
}
void FuncAnalysis::calStackVar() {
  int numDecl = 0;
  int numUninit = 0;
  //    DebugInfoFinder Finder;
  //  Finder.processModule(M);

  std::set<long> storeLines;
  for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i) {
    llvm::Instruction *Ins = &*i;
    if (auto *SI = dyn_cast<StoreInst>(Ins)) {
      if (DILocation *Loc = Ins->getDebugLoc()) {
        long storeLine = Loc->getLine();
        storeLines.insert(storeLine);
      }
    }
  }
  for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i) {
    llvm::Instruction *Ins = &*i;

    if (auto *CI = dyn_cast<CallInst>(Ins)) {
      if (!CI->getCalledFunction() || !CI->getCalledFunction()->isIntrinsic())
        continue;
      std::string fName = CI->getCalledFunction()->getName().str();
      if (fName == "llvm.dbg.declare") {
        numDecl++;
        long decLine = -1;
        if (DILocation *Loc = Ins->getDebugLoc()) {
          decLine = Loc->getLine();
        }
        if (storeLines.find(decLine) == storeLines.end()) {
          numUninit++;
        }
      }
    }
  }
  fSummary.setStackVar(numDecl);
  fSummary.setUninitStackVar(numUninit);
}
