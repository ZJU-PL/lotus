//===----------------------------------------------------------------------===//
//
// AnalysisState implementation.
// Handles intraprocedural transfer, symbolic memory management, path-condition
// queries, and affine expression evaluation.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/Engine/AnalysisState.h"

#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyInteger.h"
#include "SymbolicExecution/Domain/PropertySym.h"
#include "SymbolicExecution/Engine/AnalysisDriver.h"
#include "SymbolicExecution/Models/MemoryAPI.h"
#include "SymbolicExecution/Support/HashUtil.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>

using namespace llvm;
using namespace SymbolicExecution;

namespace {

bool isGlobalLLVMVal(const ProgramValuePtr &V) {
  return V.isa<GuardedValueFlowNodeValue>() &&
         isa<GlobalVariable>(V.getLLVMVal());
}

} // namespace

AnalysisState::AnalysisState(const AnalysisContext &Ctx,
                             const TaintModel &TaintSpec,
                             GuardedValueFlowGraph *Graph, Function *Func,
                             StateExtension *Ext)
    : Ctx(Ctx), TaintSpec(TaintSpec), Graph(Graph), F(Func), Ext(Ext),
      Solver(std::make_unique<PathCondSolver>(*Ctx.getDataLayout())) {
  for (auto Iter = Graph->arg_begin(), EIter = Graph->arg_end(); Iter != EIter;
       ++Iter) {
    auto *ArgNode = *Iter;
    if (ArgNode->getType()->isPointerTy()) {
      createMemoryObject(ArgNode, PTItem::MK_SYMBOLIC);
    }
    initSymbol(ArgNode);
  }

  taintInit(Func);
}

GuardedValueFlowNode *AnalysisState::getNode(Value *V) const {
  assert(V && "getNode() expects non-null Value");
  V = V->stripPointerCasts();

  auto *N = Graph->findNode(V);
  if (!N) {
    llvm_unreachable("Unable to find GVFG node for LLVM Value");
  }
  return N;
}

void AnalysisState::createMemoryObject(const ProgramValuePtr &Ptr,
                                       PTItem::MemObjKind Kind,
                                       const PropertyValuePtr &Sz,
                                       const Condition &Cond) {
  const PTItem Pt(Ptr, Kind, 0, Sz);
  PointsTo[Ptr].addValue(Pt, Cond);
}

PropertyValuePtr AnalysisState::computeOffsets(GEPOperator *GEP) {
  assert(!GEP->getType()->isVectorTy());
  PropertySymExpr OffsetExpr;
  int64_t ConstantOffset = 0;

  Type *CurrentType = GEP->getSourceElementType();
  bool IsFirstIndex = true;
  for (Value *IndexOperand : GEP->indices()) {
    if (!IsFirstIndex) {
      if (auto *St = dyn_cast<StructType>(CurrentType)) {
        const auto *CI = cast<ConstantInt>(IndexOperand);
        uint64_t Addend =
            Ctx.getElementOffset(St, static_cast<unsigned>(CI->getZExtValue()));
        ConstantOffset += static_cast<int64_t>(Addend);
        CurrentType =
            St->getElementType(static_cast<unsigned>(CI->getZExtValue()));
        continue;
      }
    }

    Type *ElementType = CurrentType;
    if (!IsFirstIndex) {
      if (auto *ArrayTy = dyn_cast<ArrayType>(CurrentType))
        ElementType = ArrayTy->getElementType();
      else if (auto *VectorTy = dyn_cast<VectorType>(CurrentType))
        ElementType = VectorTy->getElementType();
      else if (auto *PtrTy = dyn_cast<PointerType>(CurrentType))
        ElementType = PtrTy->getPointerElementType();
      else
        assert(false && "invalid GEP type");
    }

    uint64_t ElementSize = Ctx.getTypeStoreSize(ElementType);
    auto *IndexVal = getNode(IndexOperand);
    if (auto *CI = dyn_cast<ConstantInt>(IndexVal->getLLVMValue())) {
      ConstantOffset += CI->getSExtValue() * static_cast<int64_t>(ElementSize);
    } else {
      OffsetExpr.addTerm(Var(IndexVal), ElementSize);
    }
    CurrentType = ElementType;
    IsFirstIndex = false;
  }

  if (OffsetExpr.isConstant()) {
    ConstantOffset += OffsetExpr.getAsConstant().getAsBoundInt();
    return GetProperty<PropertyInteger>(ConstantOffset);
  }
  OffsetExpr.addConstTerm(ConstantOffset);
  return GetProperty<PropertySymExpr>(OffsetExpr);
}

void AnalysisState::initSymbol(const ProgramValuePtr &V) {
  if (V.isVacuous()) {
    return;
  }

  if (Regs.count(V) && !Regs.at(V).empty()) {
    return;
  }

  PropertyValuePtr PVP(V);
  if (!PVP) {
    llvm::errs()
        << "Warning: Failed to create PropertyValuePtr in initSymbol\n";
    return;
  }

  Regs[V].addValue(PVP);
}

void AnalysisState::transfer(Instruction *Inst, AnalysisDriver &Driver) {
  if (auto *CallI = dyn_cast<CallInst>(Inst)) {
    if (Function *Callee = CallI->getCalledFunction()) {
      if (Callee->isIntrinsic()) {
        switch (Callee->getIntrinsicID()) {
        case Intrinsic::dbg_declare:
        case Intrinsic::dbg_value:
        case Intrinsic::dbg_label:
        case Intrinsic::dbg_addr:
          return;
        default:
          break;
        }
      }
    }
  }

  Function *CurFun = Inst->getParent()->getParent();
  assert(!Driver.hasSummary(CurFun));

  for (unsigned Idx = 0; Idx < Inst->getNumOperands(); ++Idx) {
    auto *Opnd = Inst->getOperand(Idx);
    if (!isa<Instruction>(Opnd)) {
      if (auto *OpndNode = Graph->findNode(Opnd))
        initSymbol(OpndNode);
      auto *Stripped = Opnd->stripPointerCasts();
      if (auto *StrippedNode = Graph->findNode(Stripped))
        initSymbol(StrippedNode);
    }
  }

  if (Ext) {
    Ext->onPreInstruction(Inst, *this);
  }

  auto OpC = Inst->getOpcode();
  switch (OpC) {
  case llvm::Instruction::UDiv:
  case llvm::Instruction::SDiv:
  case llvm::Instruction::Mul:
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub: {
    processBinaryInst(Inst);
    break;
  }
  case llvm::Instruction::FMul:
  case llvm::Instruction::FAdd:
  case llvm::Instruction::FSub:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
  case llvm::Instruction::FDiv:
  case llvm::Instruction::SRem:
  case llvm::Instruction::FRem:
  case llvm::Instruction::URem:
  case llvm::Instruction::ExtractElement:
  case llvm::Instruction::InsertElement:
  case llvm::Instruction::FCmp:
  case llvm::Instruction::Select: {
    break;
  }
  case llvm::Instruction::Shl: {
    if (auto *CI = dyn_cast<ConstantInt>(Inst->getOperand(1))) {
      if (CI->getZExtValue() < 64) {
        auto Cst = PropertyInteger(1ull << CI->getZExtValue());
        auto Res = Regs.at(getNode(Inst->getOperand(0))) * Cst;
        if (!Res.empty()) {
          Regs[getNode(Inst)] = std::move(Res);
        }
      }
    }
    break;
  }
  case llvm::Instruction::ICmp: {
    processICmpInst(Inst);
    break;
  }
  case Instruction::Alloca: {
    processAlloca(Inst);
    break;
  }
  case llvm::Instruction::AddrSpaceCast:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::BitCast:
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::Trunc:
  case llvm::Instruction::FPTrunc:
  case llvm::Instruction::FPExt:
  case llvm::Instruction::SIToFP:
  case llvm::Instruction::FPToSI:
  case llvm::Instruction::UIToFP:
  case llvm::Instruction::FPToUI: {
    assignVal(getNode(Inst), getNode(Inst->getOperand(0)->stripPointerCasts()));
    break;
  }
  case Instruction::GetElementPtr: {
    processGEP(Inst);
    break;
  }
  case Instruction::Load: {
    processLoad(Inst);
    break;
  }
  case Instruction::Store: {
    processStore(Inst);
    break;
  }
  case Instruction::PHI: {
    processPhiInst(Inst);
    break;
  }
  case llvm::Instruction::Call: {
    Function *Callee = getCallee(Inst);
    auto *CallI = cast<CallInst>(Inst);

    auto AllocSizeArgsIdx = getHeapAllocSizeArgs(CallI);
    if (!AllocSizeArgsIdx.empty()) {
      std::vector<unsigned> AllocSizeArgs;
      AllocSizeArgs.reserve(AllocSizeArgsIdx.size());
      for (int I : AllocSizeArgsIdx) {
        if (I >= 0) {
          AllocSizeArgs.push_back(static_cast<unsigned>(I));
        }
      }
      processMemAlloc(CallI, AllocSizeArgs);
    } else if (!isDefiniteCall(Inst)) {
      processLibraryCall(CallI);
    } else if (Driver.hasSummary(Callee)) {
      unsigned FunDepth = Ctx.getFunctionDepth(Callee);
      if (FunDepth > AnalysisLimit::FUNC_INLINE_LIMIT_V) {
        processAsUnknownLib(CallI);
      } else {
        processCall(CallI, Callee, Driver.getSummary(Callee));
      }
    }
    break;
  }
  case llvm::Instruction::Ret: {
    this->RetI = cast<ReturnInst>(Inst);
    break;
  }
  default:
    break;
  }

  if (!Inst->getType()->isVoidTy()) {
    initSymbol(getNode(Inst));
  }

  taintTransfer(Inst);

  if (Ext) {
    Ext->onPostInstruction(Inst, *this);
  }
}

void AnalysisState::processBinaryInst(Instruction *Inst) {
  auto OpC = PropertyValue::fromLLVMOp(Inst->getOpcode());
  auto Res = Regs.at(getNode(Inst->getOperand(0)))
                 .binOp(Regs.at(getNode(Inst->getOperand(1))), OpC);
  if (!Res.empty()) {
    Regs[getNode(Inst)] = std::move(Res);
  }
}

void AnalysisState::processICmpInst(Instruction *Inst) {
  auto *CmpI = cast<CmpInst>(Inst);
  unsigned Pred = CmpI->getPredicate();
  auto Res = Regs.at(getNode(Inst->getOperand(0)))
                 .cmp(Regs.at(getNode(Inst->getOperand(1))), Pred);

  auto *Dst = getNode(Inst);
  Regs[Dst].addValue(GetProperty<PropertyInteger>(0), Res[0]);
  Regs[Dst].addValue(GetProperty<PropertyInteger>(1), Res[1]);
}

void AnalysisState::processAlloca(Instruction *Inst) {
  auto *AllocI = cast<AllocaInst>(Inst);
  Type *AllocatedTy = AllocI->getAllocatedType();
  auto *NumElements = getNode(AllocI->getArraySize());
  auto *Dst = getNode(Inst);

  uint64_t TySize = Ctx.getTypeStoreSizeInBits(AllocatedTy);
  if (TySize == 0) {
    TySize = 8;
  }

  ProgramValuePtr NumV(NumElements);
  if (NumV.isVacuous()) {
    createMemoryObject(Dst, PTItem::MK_PLACEHOLDER);
    return;
  }

  if (!NumV.isConstant() && !Regs.count(NumElements)) {
    initSymbol(NumElements);
    if (!Regs.count(NumElements)) {
      createMemoryObject(Dst, PTItem::MK_PLACEHOLDER);
      return;
    }
  }
  if (NumV.isConstant()) {
    BigInteger Sz = NumV.getAsConstant() * TySize;
    PropertyValuePtr AllocSize = GetProperty<PropertyInteger>(std::move(Sz));
    createMemoryObject(Dst, PTItem::MK_CONCRETE, AllocSize);
  } else {
    auto SizeVals = Regs.at(NumElements) * PropertyInteger(TySize);
    for (const auto &SizeCond : SizeVals) {
      createMemoryObject(Dst, PTItem::MK_CONCRETE, SizeCond.first,
                         SizeCond.second);
    }
  }
  assert(!PointsTo.at(Dst).empty());
}

void AnalysisState::setPts(const ProgramValuePtr &Dst, const PtsSet &Pts) {
  PointsTo[Dst] = Pts;
}

void AnalysisState::assignVal(const ProgramValuePtr &Dst,
                              const ProgramValuePtr &Src,
                              const Condition &Cond) {
  if (!Regs.count(Src)) {
    initSymbol(Src);
  }
  initSymbol(Dst);

  if (Regs.count(Src)) {
    Regs[Dst].addValues(Regs.at(Src), Cond);
  }

  if (PointsTo.count(Src)) {
    PointsTo[Dst].addValues(PointsTo.at(Src), Cond);
  }
}

void AnalysisState::assignVals(const ProgramValuePtr &Dst,
                               const GuardedProgramValSet &SrcVals) {
  assert(!Regs.count(Dst));
  assert(!PointsTo.count(Dst));
  for (const auto &P : SrcVals) {
    const auto &V = P.first;
    const auto &Cond = P.second;
    initSymbol(V);

    Regs[Dst].addValues(Regs.at(V), Cond);
    if (PointsTo.count(V)) {
      PointsTo[Dst].addValues(PointsTo.at(V), Cond);
    }
  }
}

void AnalysisState::assignPtr(const ProgramValuePtr &Dst,
                              const ProgramValuePtr &Src,
                              const GuardedSymbolicValSet &Offset,
                              bool StrongUpdate, const Condition &AssignCond) {
  if (!PointsTo.count(Src)) {
    return;
  }

  auto &DstPts = PointsTo[Dst];
  if (StrongUpdate) {
    DstPts.clear();
  }

  if (DstPts.isFull()) {
    return;
  }

  const auto &SrcPts = PointsTo.at(Src);
  SrcPts.forEach2(
      Offset,
      [&](const PTItem &Target, const PropertyValuePtr &OffVal,
          const Condition &SrcCond) {
        DstPts.addValue(Target.offsetBy(OffVal), SrcCond && AssignCond);
      },
      [&] { return DstPts.isFull(); });
}

void AnalysisState::processGEP(Instruction *Inst) {
  if (!Inst->getType()->isPointerTy()) {
    return;
  }
  auto *GEP = cast<GEPOperator>(Inst);
  PropertyValuePtr Off = computeOffsets(GEP);
  auto OffsetVals = evalExpr(Off, Regs);
  Condition IndexDataDeps;
  if (IsaProperty<PropertySymExpr>(Off)) {
    const auto *SymE = CastProperty<PropertySymExpr>(Off);
    for (const auto &Entry : *SymE) {
      ProgramValuePtr X = Entry.first.getValue();
      if (X.isa<GuardedValueFlowNodeValue>()) {
        auto *N = X.getAs<GuardedValueFlowNodeValue>()->getNode();
        IndexDataDeps = IndexDataDeps && getDataDepsCond(N);
      }
    }
  }
  OffsetVals = OffsetVals && IndexDataDeps;

  auto *SrcPtr = getNode(getPointerOperand(GEP));
  initPointsToTarget(SrcPtr, Inst);

  auto OffsetInBits = OffsetVals * PropertyInteger(8);
  auto *Dst = getNode(Inst);

  if (Dst != SrcPtr) {
    assignPtr(Dst, SrcPtr, OffsetInBits, true);
    if (PointsTo.count(Dst) && PointsTo.at(Dst).empty()) {
      assignPtr(Dst, SrcPtr, Off * PropertyInteger(8), true);
    }
  }

  Regs[Dst] = Regs.at(SrcPtr) + OffsetVals;
}

void AnalysisState::initPointsToTarget(const ProgramValuePtr &Ptr,
                                       Instruction *Pos) {
  (void)Pos;
  if (PointsTo.count(Ptr) && !PointsTo.at(Ptr).empty()) {
    return;
  }

  if (isGlobalLLVMVal(Ptr) || isPseudoArgVal(Ptr)) {
    createMemoryObject(Ptr, PTItem::MK_SYMBOLIC);
  } else {
    createMemoryObject(Ptr, PTItem::MK_PLACEHOLDER);
  }

  initSymbol(Ptr);
  assert(!PointsTo.at(Ptr).empty());
}

void AnalysisState::processLoad(Instruction *Inst) {
  auto *LoadI = cast<LoadInst>(Inst);
  auto *LdPtr = getNode(getPointerOperand(LoadI));
  initPointsToTarget(LdPtr, Inst);

  auto *LdInstNode = getNode(Inst);
  if (LdInstNode->getNumChildren() >= 1) {
    if (auto *LdMemNode =
            dyn_cast<GuardedValueFlowNode>(LdInstNode->getChild(0))) {
      processLoadPtr(LdPtr, LdMemNode, LdInstNode, Inst);
    }
  }
}

void AnalysisState::processLoadPtr(const ProgramValuePtr &Ptr,
                                   const GuardedValueFlowNode *LdMemNode,
                                   const ProgramValuePtr &Dst,
                                   Instruction *Pos) {
  if (!Ptr.isNull()) {
    initSymbol(Ptr);
  }

  auto Incomings = getIncomingValuesForLoad(LdMemNode);
  for (auto &Item : Incomings) {
    Condition Cond = getRegionCond(Item.second);

    initSymbol(Item.first);
    assignVal(Dst, Item.first, Cond);

    if (!Ptr.isNull()) {
      propagateTaintPointer(Ptr, Item.first, Pos, Cond);
    }

    if (hasSymbolicVals(Dst)) {
      propagateTaint(Item.first, Dst, Pos, Cond, false);
    }
  }
}

void AnalysisState::collectEscapeObjs() {
  // Conservative no-op until GVFG exposes escaping allocation queries.
}

void AnalysisState::processReturn() {
  auto *RetNode = Graph->getCommonReturn();
  if (RetNode) {
    if (RetNode->getNumChildren() >= 1) {
      auto *RetValNode = cast<GuardedValueFlowNode>(RetNode->getChild(0));
      initSymbol(RetValNode);
      assignVal(RetNode, RetValNode);
      propagateTaint(RetValNode, RetNode, nullptr, Condition(), false);
    } else {
      initSymbol(RetNode);
    }
    OutSymbolicValMap[RetNode].addValues(Regs.at(RetNode));
  }

  for (auto Iter = Graph->pseudo_return_begin(),
            EIter = Graph->pseudo_return_end();
       Iter != EIter; ++Iter) {
    auto *PseudoRet = cast<GuardedValueFlowReturnNode>(*Iter);
    Value *BasePtr = PseudoRet->getAccessPath().getBase();
    ProgramValuePtr LdPtr = BasePtr ? getNode(BasePtr) : ProgramValuePtr();

    for (size_t I = 0; I < PseudoRet->getNumChildren(); ++I) {
      auto *Ch = PseudoRet->getChild(I);
      if (auto *LdMemNode = dyn_cast<GuardedValueFlowNode>(Ch)) {
        processLoadPtr(LdPtr, LdMemNode, PseudoRet, RetI);
      }
    }

    initSymbol(PseudoRet);
    OutSymbolicValMap[PseudoRet].addValues(Regs.at(PseudoRet));
  }

  for (auto Iter = Graph->return_begin(), EIter = Graph->return_end();
       Iter != EIter; ++Iter) {
    auto *RNode = *Iter;
    if (PointsTo.count(RNode)) {
      OutputPts[RNode] = PointsTo.at(RNode);
    }
  }

  collectEscapeObjs();
}

void AnalysisState::processStore(Instruction *Inst) {
  auto *StoreI = cast<StoreInst>(Inst);
  ProgramValuePtr StPtr = getNode(getPointerOperand(StoreI));
  initPointsToTarget(StPtr, Inst);
}

void AnalysisState::processPhiInst(Instruction *Inst) {
  auto *Dst = cast<GuardedValueFlowPhiNode>(getNode(Inst));
  for (const auto &InNode : *Dst) {
    auto PhiCond = getPhiCond(Dst, InNode);
    assignVal(Dst, cast<GuardedValueFlowNode>(InNode.value_node), PhiCond);
  }
}

void AnalysisState::processMemAlloc(
    CallInst *Inst, const std::vector<unsigned> &AllocSizeArgs) {
  if (AllocSizeArgs.size() <= 1) {
    handleMalloc(Inst, AllocSizeArgs[0]);
  } else {
    handleCalloc(Inst, AllocSizeArgs[0], AllocSizeArgs[1]);
  }
}

void AnalysisState::handleMalloc(CallInst *Inst, unsigned SzIdx) {
  auto *SzNode = getNode(Inst->getArgOperand(SzIdx));
  if (!Regs.count(SzNode)) {
    initSymbol(SzNode);
    if (!Regs.count(SzNode)) {
      createMemoryObject(getNode(Inst), PTItem::MK_PLACEHOLDER);
      return;
    }
  }
  auto SzVals = Regs.at(SzNode) * PropertyInteger(8);
  auto *Dst = getNode(Inst);
  for (const auto &P : SzVals) {
    if (P.first < int64_t(0)) {
      continue;
    }
    createMemoryObject(Dst, PTItem::MK_CONCRETE, P.first, P.second);
  }

  initPointsToTarget(Dst, Inst);
}

void AnalysisState::handleCalloc(CallInst *Inst, unsigned NumIdx,
                                 unsigned SzIdx) {
  auto *NumNode = getNode(Inst->getArgOperand(NumIdx));
  auto *SzNode = getNode(Inst->getArgOperand(SzIdx));
  if (!Regs.count(NumNode)) {
    initSymbol(NumNode);
  }
  if (!Regs.count(SzNode)) {
    initSymbol(SzNode);
  }
  if (!Regs.count(NumNode) || !Regs.count(SzNode)) {
    createMemoryObject(getNode(Inst), PTItem::MK_PLACEHOLDER);
    return;
  }

  auto SzVals = Regs.at(NumNode) * PropertyInteger(8) * Regs.at(SzNode);
  auto *Dst = getNode(Inst);
  for (const auto &P : SzVals) {
    createMemoryObject(Dst, PTItem::MK_CONCRETE, P.first, P.second);
  }

  initPointsToTarget(Dst, Inst);
}

void AnalysisState::processFreeCall(CallInst *Inst) {
  if (Inst->arg_size() < 1)
    return;

  Value *Ptr = Inst->getArgOperand(0);
  auto *PtrNode = getNode(Ptr);
  Condition BBCond = getLocalCond(Inst->getParent());

  FreedPtrSet.addValue(PtrNode, BBCond);

  if (hasPts(PtrNode)) {
    const PtsSet &Pts = getPts(PtrNode);
    Pts.forEach([&](const PTItem &Pt, const Condition &Cond) {
      FreedPtrSet.addValue(Pt.getAllocSite(), BBCond && Cond);
    });
  }
}

void AnalysisState::processLibraryCall(CallInst *Inst) {
  Function *Callee = Inst->getCalledFunction();
  if (!Callee || !Callee->isDeclaration()) {
    return;
  }

  if (isFreeLikeFunction(Callee)) {
    processFreeCall(Inst);
    return;
  }

  GuardedValueFlowNode *Dst = nullptr;
  if (!Inst->getType()->isVoidTy()) {
    Dst = getNode(Inst);
  }

  auto GetRegsSafe = [&](GuardedValueFlowNode *Node) -> GuardedSymbolicValSet & {
    if (!Node) {
      return Regs[ProgramValuePtr()];
    }
    if (!Regs.count(Node)) {
      initSymbol(Node);
    }
    if (!Regs.count(Node)) {
      return Regs[ProgramValuePtr(Node)];
    }
    return Regs.at(Node);
  };

  if (Callee->isIntrinsic()) {
    auto IntrinsicID = Callee->getIntrinsicID();
    if (IntrinsicID == Intrinsic::expect) {
      if (Inst->arg_size() >= 1) {
        auto *Arg0 = getNode(Inst->getArgOperand(0));
        if (!Regs.count(Arg0)) {
          initSymbol(Arg0);
        }
        Regs[Dst].setValues(GetRegsSafe(Arg0));
      }
    } else if (IntrinsicID == Intrinsic::umul_with_overflow) {
      auto *Arg0 = getNode(Inst->getArgOperand(0));
      auto *Arg1 = getNode(Inst->getArgOperand(1));
      if (!Regs.count(Arg0)) {
        initSymbol(Arg0);
      }
      if (!Regs.count(Arg1)) {
        initSymbol(Arg1);
      }
      if (!Regs.count(Arg0) || !Regs.count(Arg1)) {
        createMemoryObject(Dst, PTItem::MK_PLACEHOLDER);
        return;
      }
      auto Res = GetRegsSafe(Arg0) * GetRegsSafe(Arg1);
      if (!Res.empty()) {
        Regs[Dst] = std::move(Res);
      }
    }
    return;
  }

  Condition BBCond = getLocalCond(Inst->getParent());
  auto HandleStringLen = [&](const ProgramValuePtr &Ptr, bool IsDirect) {
    StrState.handleCStrLen(Ptr, Inst, BBCond, *this, IsDirect);
  };

  const std::string CalleeName = Callee->getName().str();
  if (isMatchLib(Inst, CalleeName, "strlen")) {
    HandleStringLen(getNode(Inst->getArgOperand(0)), true);
    return;
  }

  if (isMatchLib(Inst, CalleeName, "puts")) {
    HandleStringLen(getNode(Inst->getArgOperand(0)), false);
    return;
  }

  if (isMatchLib(Inst, CalleeName, "strcpy") ||
      isMatchLib(Inst, CalleeName, "strcat") ||
      isMatchLib(Inst, CalleeName, "strncat")) {
    HandleStringLen(getNode(Inst->getArgOperand(0)), false);
    HandleStringLen(getNode(Inst->getArgOperand(1)), false);
    return;
  }

  if (isPureLibrary(Callee) || isKnownLib(CalleeName)) {
    return;
  }

  processAsUnknownLib(Inst);
}

void AnalysisState::processAsUnknownLib(CallInst *Inst) {
  for (unsigned Idx = 0; Idx < Inst->arg_size(); ++Idx) {
    Value *Arg = Inst->getArgOperand(Idx);
    if (auto *ArgTy = dyn_cast<PointerType>(Arg->getType())) {
      if (ArgTy->getPointerElementType()->isAggregateType()) {
        continue;
      }
      initPointsToTarget(getNode(Arg), Inst);
    }
  }

  if (Inst->getType()->isPointerTy()) {
    initPointsToTarget(getNode(Inst), Inst);
  }
}

Condition AnalysisState::getLocalCond(BasicBlock *BB) const {
  auto It = LocalCondMap.find(BB);
  if (It != LocalCondMap.end()) {
    return It->second;
  }

  Condition BBCond(Solver->getCtrlDeps(BB, Graph), Solver.get());
  BBCond = BBCond && getCallSiteOutDeps();
  LocalCondMap.emplace(BB, BBCond);
  return BBCond;
}

Condition AnalysisState::getDataDepsCond(GuardedValueFlowNode *N) const {
  auto It = DataDepsCondMap.find(N);
  if (It != DataDepsCondMap.end()) {
    return It->second;
  }

  Condition CurDeps(Solver->getDataDeps(N), Solver.get());
  CurDeps = CurDeps && getCallSiteOutDeps();
  DataDepsCondMap.emplace(N, CurDeps);
  return CurDeps;
}

Condition AnalysisState::getCallSiteOutDeps() const {
  auto UsedCSOuts = Solver->getUsedCallSiteOutput();
  Condition ResCond = Condition::getTrueCond();
  for (auto *CSO : UsedCSOuts) {
    auto It = CSOutputCondMap.find(CSO);
    if (It != CSOutputCondMap.end()) {
      ResCond = ResCond && It->second;
      continue;
    }

    if (Regs.count(CSO)) {
      Condition CurCond(
          buildVarEqValues(CSO, Solver->getExpr(Var(CSO)), Regs.at(CSO)),
          Solver.get());
      CSOutputCondMap.emplace(CSO, CurCond);
      ResCond = ResCond && CurCond;
    }
  }
  return ResCond;
}

Condition AnalysisState::getRegionCond(GuardedValueFlowRegionNode *R) const {
  auto It = RegionCondMap.find(R);
  if (It != RegionCondMap.end()) {
    return It->second;
  }

  Condition Cond(R, getSolver());
  Cond = Cond && getCallSiteOutDeps();
  RegionCondMap.emplace(R, Cond);
  return Cond;
}

Condition AnalysisState::getPhiCond(
    const GuardedValueFlowPhiNode *PhiNode,
    const GuardedValueFlowPhiNode::Incoming InNode) const {
  auto PhiCond = Solver->getPhiGated(PhiNode, InNode);
  Condition ResCond(PhiCond, Solver.get());
  return ResCond && getCallSiteOutDeps();
}

GuardedSymbolicValSet AnalysisState::inlineExpr(const PropertyValuePtr &E,
                                                Instruction *CS) const {
  if (IsaProperty<PropertyInteger>(E)) {
    return E;
  }

  if (InlineExprCache.count(E) && InlineExprCache.at(E).count(CS)) {
    return InlineExprCache.at(E).at(CS);
  }

  auto Res = evalExpr(E, FormalToRealSymValsMap.at(CS), CS);
  InlineExprCache[E][CS] = Res;
  return Res;
}

ProgramValuePtr AnalysisState::getFreeVar(Type *Ty) const {
  std::string Name =
      "free_" + ptrToString(F) + "_" + std::to_string(FreeVarID++);
  return {Ty, std::move(Name)};
}

GuardedSymbolicValSet AnalysisState::evalExpr(
    const PropertyValuePtr &E,
    const std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> &M,
    Instruction *CS) const {
  if (IsaProperty<PropertyInteger>(E)) {
    return E;
  }

  std::vector<GuardedSymbolicValSet> FreeVarStorage;
  GuardedSymbolicValSet Result;
  std::vector<
      std::tuple<GuardedSymbolicValSet::IterType,
                 GuardedSymbolicValSet::IterType, BigInteger, ProgramValuePtr>>
      Seq;
  const auto *SymE = CastProperty<PropertySymExpr>(E);

  PropertyValuePtr Residual = GetProperty<PropertyInteger>(SymE->getOffsets());
  for (const auto &Entry : *SymE) {
    ProgramValuePtr X = Entry.first.getValue();
    BigInteger C = Entry.second;

    if (M.count(X) && !M.at(X).empty()) {
      const auto &ValSet = M.at(X);
      Seq.emplace_back(ValSet.begin(), ValSet.end(), C, X);
    } else {
      FreeVarStorage.emplace_back(
          GetProperty<PropertySymExpr>(Var(getFreeVar(X.getType())), C));
      const auto &ValSet = FreeVarStorage.back();
      Seq.emplace_back(ValSet.begin(), ValSet.end(), C, X);
    }
  }

  if (Seq.empty()) {
    Result.addValue(Residual);
  } else {
    std::string Suffix = CS ? getCallsiteSuffix(CS) : std::string();
    Result.addValues(
        GuardedSymbolicValSet::seqAdd(Seq, Solver.get(), true, Suffix) +
        Residual);
  }

  return Result;
}

std::string AnalysisState::getCallsiteSuffix(Instruction *CS) const {
  return "_" + ptrToString(CS);
}

SMTExpr
AnalysisState::buildVarEqValues(const ProgramValuePtr &V, SMTExpr VExpr,
                                const GuardedSymbolicValSet &SymVals) const {
  auto LSz = VExpr.getBitVecSize();
  SMTExprVec ValVec = Solver->createEmptySMTExprVec();

  SymVals.forEach([&](const PropertyValuePtr &Val, const Condition &ValCond) {
    if (ValCond.isFalse()) {
      return;
    }

    if (IsaProperty<PropertySymExpr>(Val) &&
        CastProperty<PropertySymExpr>(Val)->isVar() &&
        CastProperty<PropertySymExpr>(Val)->getAsVar().getValue() == V) {
      return;
    }

    SMTExpr ValCondExpr =
        ValCond.isTrue() ? Solver->buildBoolVal(true) : ValCond.getSMTConstr();
    SMTExpr ValExpr = Solver->buildExprForVal(Val.get());
    assert(ValExpr.isBitVector());

    unsigned RSz = ValExpr.getBitVecSize();
    if (LSz == RSz) {
      ValVec.push_back((VExpr == ValExpr) && ValCondExpr);
    } else if (LSz > RSz) {
      auto Temp = ValExpr.basic_sext(LSz - RSz);
      assert(Temp.getBitVecSize() == LSz);
      ValVec.push_back((VExpr == Temp) && ValCondExpr);
    } else {
      auto Temp = ValExpr.basic_extract(LSz - 1, 0);
      assert(Temp.getBitVecSize() == LSz);
      ValVec.push_back((VExpr == Temp) && ValCondExpr);
    }
  });

  return ValVec.toOrExpr();
}

Condition AnalysisState::getMappingCond(Instruction *CS,
                                        Function *Callee) const {
  auto It = MappingCondCache.find(CS);
  if (It != MappingCondCache.end()) {
    return It->second;
  }

  std::string CSSuffix = getCallsiteSuffix(CS);
  SMTExprVec Vec = Solver->createEmptySMTExprVec();
  std::vector<Condition> RealDataDeps;

  bool IsComplete = true;
  for (const auto &Iter : SMTRenameCtxMap.at(Callee)) {
    if (!Iter.second.hasRealExpr(CS)) {
      IsComplete = false;
      continue;
    }

    ProgramValuePtr Formal(Iter.first);
    SMTExpr FormalExpr =
        Solver->renameExpr(Iter.second.getFormalExpr(), CSSuffix);

    ProgramValuePtr Real = getRealForFormal(CS, Formal);
    SMTExpr RealExpr = Iter.second.getRealExpr(CS);

    Vec.push_back(FormalExpr == RealExpr);

    if (!Real.isConstant()) {
      if (Real.isa<GuardedValueFlowNodeValue>() &&
          Real.getAs<GuardedValueFlowNodeValue>()->getNode()) {
        RealDataDeps.emplace_back(getDataDepsCond(
            Real.getAs<GuardedValueFlowNodeValue>()->getNode()));
      } else {
        const auto &RealSymVals = FormalToRealSymValsMap.at(CS).at(Formal);
        Vec.push_back(buildVarEqValues(Real, RealExpr, RealSymVals));
      }
    }
  }

  Condition ResCond(Vec.toAndExpr(), Solver.get());
  for (const auto &DepsCond : RealDataDeps) {
    ResCond = ResCond && DepsCond;
  }

  if (IsComplete) {
    MappingCondCache.emplace(CS, ResCond);
  }

  return ResCond;
}

bool AnalysisState::isInstUnmodelled(Instruction *Inst) const {
  switch (Inst->getOpcode()) {
  case llvm::Instruction::FMul:
  case llvm::Instruction::FAdd:
  case llvm::Instruction::FSub:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
  case llvm::Instruction::Shl:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
  case llvm::Instruction::FDiv:
  case llvm::Instruction::SRem:
  case llvm::Instruction::FRem:
  case llvm::Instruction::URem:
  case llvm::Instruction::ExtractElement:
  case llvm::Instruction::InsertElement:
  case llvm::Instruction::FCmp:
  case llvm::Instruction::Select:
    return true;
  default:
    return false;
  }
}

bool AnalysisState::mustBeConstantInt(const ProgramValuePtr &V) const {
  if (V.isa<GuardedValueFlowNodeValue>() &&
      isa<ConstantPointerNull>(V.getLLVMVal())) {
    return false;
  }

  if (!hasPts(V)) {
    for (const auto &Pair : getSymbolicVals(V)) {
      if (!IsaProperty<PropertyInteger>(Pair.first)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool SymbolicExecution::isPseudoArgVal(const ProgramValuePtr &V) {
  if (V.isa<GuardedValueFlowNodeValue>()) {
    auto *NV = V.getAs<GuardedValueFlowNodeValue>();
    return isa<lotus::gvfg::GuardedValueFlowArgumentNode>(NV->getNode());
  }
  return false;
}
