#include "Analysis/CallGraph/VirtualCallUtils.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/Casting.h"

namespace lotus {

std::optional<unsigned> getVFTIndex(const llvm::CallBase *CallSite) {
  const auto *Load =
      llvm::dyn_cast_or_null<llvm::LoadInst>(CallSite->getCalledOperand());
  if (!Load) {
    return std::nullopt;
  }
  const auto *GEP =
      llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(Load->getPointerOperand());
  if (!GEP) {
    return std::nullopt;
  }
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1))) {
    return CI->getZExtValue();
  }
  return std::nullopt;
}

std::optional<std::pair<const llvm::Value *, uint64_t>>
getVFTIndexAndVT(const llvm::CallBase *CallSite) {
  const auto *Load =
      llvm::dyn_cast_or_null<llvm::LoadInst>(CallSite->getCalledOperand());
  if (!Load) {
    return std::nullopt;
  }

  const auto *GEP =
      llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(Load->getPointerOperand());
  if (!GEP || GEP->getNumOperands() != 2) {
    return std::nullopt;
  }

  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1))) {
    return {{GEP->getPointerOperand(), CI->getZExtValue()}};
  }

  return std::nullopt;
}

std::optional<std::tuple<const llvm::Value *, llvm::SmallVector<uint64_t, 3>,
                         llvm::Type *>>
getConstGEPFieldAccess(const llvm::Value *PtrOperand) {
  const auto *GEP = llvm::dyn_cast_or_null<llvm::GEPOperator>(PtrOperand);
  if (!GEP || GEP->getNumOperands() < 3 || !GEP->hasAllConstantIndices()) {
    return std::nullopt;
  }
  llvm::SmallVector<uint64_t, 3> Indices;
  for (const llvm::Use &Idx : GEP->indices()) {
    Indices.push_back(llvm::cast<llvm::ConstantInt>(Idx.get())->getZExtValue());
  }
  return {{GEP->getPointerOperand(), std::move(Indices),
           GEP->getSourceElementType()}};
}

std::optional<std::tuple<const llvm::Value *, llvm::SmallVector<uint64_t, 3>,
                         llvm::Type *>>
getStructVCallInfo(const llvm::CallBase *CallSite) {
  const auto *Load =
      llvm::dyn_cast_or_null<llvm::LoadInst>(CallSite->getCalledOperand());
  if (!Load) {
    return std::nullopt;
  }
  return getConstGEPFieldAccess(Load->getPointerOperand());
}

static bool isTypeMatchForFunctionArgument(llvm::Type *Actual,
                                           llvm::Type *Formal) {
  if (Actual == Formal) {
    return true;
  }
  if (Actual->getTypeID() != Formal->getTypeID()) {
    return false;
  }
  if (llvm::isa<llvm::PointerType>(Actual)) {
    return true;
  }
  return false;
}

bool isConsistentCall(const llvm::CallBase *CallSite,
                      const llvm::Function *DestFun) {
  if (CallSite->arg_size() < DestFun->arg_size()) {
    return false;
  }
  if (CallSite->arg_size() != DestFun->arg_size() && !DestFun->isVarArg()) {
    return false;
  }

  for (const auto &[Param, ArgOp] :
       llvm::zip(DestFun->args(), CallSite->args())) {
    const auto *ParamTy = Param.getType();
    const auto *ArgTy = ArgOp->getType();

    if (ParamTy == ArgTy) {
      continue;
    }

    if (!isTypeMatchForFunctionArgument(const_cast<llvm::Type *>(ArgTy),
                                        const_cast<llvm::Type *>(ParamTy))) {
      return false;
    }
  }

  const auto *CallRetTy = CallSite->getType();
  const auto *DestRetTy = DestFun->getReturnType();

  if (CallRetTy == DestRetTy) {
    return true;
  }

  return isTypeMatchForFunctionArgument(const_cast<llvm::Type *>(DestRetTy),
                                        const_cast<llvm::Type *>(CallRetTy));
}

static llvm::DbgVariableIntrinsic *getDbgVarIntrinsic(const llvm::Value *V) {
  if (auto *VAM = llvm::ValueAsMetadata::getIfExists(
          const_cast<llvm::Value *>(V))) {
    if (auto *MDV = llvm::MetadataAsValue::getIfExists(V->getContext(), VAM)) {
      for (auto *U : MDV->users()) {
        if (auto *DBGIntr = llvm::dyn_cast<llvm::DbgVariableIntrinsic>(U)) {
          return DBGIntr;
        }
      }
    }
  } else if (const auto *Arg = llvm::dyn_cast<llvm::Argument>(V)) {
    for (const auto *User : Arg->users()) {
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
        if (Store->getValueOperand() == Arg &&
            llvm::isa<llvm::AllocaInst>(Store->getPointerOperand())) {
          return getDbgVarIntrinsic(Store->getPointerOperand());
        }
      }
    }
  }
  return nullptr;
}

llvm::DILocalVariable *getDILocalVariable(const llvm::Value *V) {
  if (auto *DbgIntr = getDbgVarIntrinsic(V)) {
    if (auto *DDI = llvm::dyn_cast<llvm::DbgDeclareInst>(DbgIntr)) {
      return DDI->getVariable();
    }
    if (auto *DVI = llvm::dyn_cast<llvm::DbgValueInst>(DbgIntr)) {
      return DVI->getVariable();
    }
  }
  return nullptr;
}

static llvm::DIGlobalVariable *getDIGlobalVariable(const llvm::Value *V) {
  if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
    if (auto *MN = GV->getMetadata(llvm::LLVMContext::MD_dbg)) {
      if (auto *DIGVExp = llvm::dyn_cast<llvm::DIGlobalVariableExpression>(MN)) {
        return DIGVExp->getVariable();
      }
    }
  }
  return nullptr;
}

static llvm::DIType *getVarTypeFromIRImpl(const llvm::Value *V) {
  if (auto *LocVar = getDILocalVariable(V)) {
    return LocVar->getType();
  }
  if (auto *GlobVar = getDIGlobalVariable(V)) {
    return GlobVar->getType();
  }
  if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(V)) {
    if (const auto *Callee = llvm::dyn_cast<llvm::Function>(
            Call->getCalledOperand()->stripPointerCastsAndAliases())) {
      if (auto *DICallee = Callee->getSubprogram()) {
        auto Types = DICallee->getType()->getTypeArray();
        if (Types.size()) {
          return Types[0];
        }
      }
    }
  }
  return nullptr;
}

static const llvm::GEPOperator *getStructGep(const llvm::Value *V) {
  if (const auto *Gep = llvm::dyn_cast<llvm::GEPOperator>(V)) {
    if (Gep->getNumIndices() != 2) {
      return nullptr;
    }
    const auto *FirstIdx =
        llvm::dyn_cast<llvm::ConstantInt>(Gep->indices().begin()->get());
    if (!FirstIdx || FirstIdx->getZExtValue() != 0) {
      return nullptr;
    }

    const auto *SecondIdx = llvm::dyn_cast<llvm::ConstantInt>(
        std::next(Gep->indices().begin())->get());
    if (!SecondIdx) {
      return nullptr;
    }
    return Gep;
  }
  return nullptr;
}

static std::pair<const llvm::Value *, size_t>
getOffsetAndBase(const llvm::Value *V) {
  const auto *Base = V->stripPointerCastsAndAliases();
  uint64_t Offset = 0;
  if (const auto *Gep = getStructGep(Base)) {
    const auto *SecondIdx =
        llvm::cast<llvm::ConstantInt>(std::next(Gep->indices().begin())->get());
    Offset = SecondIdx->getZExtValue();
    Base = Gep->getPointerOperand();
  }
  return {Base, Offset};
}

static llvm::DIType *getStructElementType(llvm::DIType *BaseTy, size_t Offset) {
  const auto *DerivedTy = llvm::dyn_cast_or_null<llvm::DIDerivedType>(BaseTy);
  auto *StructTy = DerivedTy ? DerivedTy->getBaseType() : BaseTy;

  if (Offset == 0 && DerivedTy) {
    return StructTy;
  }

  if (const auto *CompositeTy =
          llvm::dyn_cast_or_null<llvm::DICompositeType>(StructTy)) {
    auto Elems = CompositeTy->getElements();
    if (!Elems || Offset >= Elems.size()) {
      return nullptr;
    }

    if (auto *ElemTy = llvm::dyn_cast_or_null<llvm::DIType>(Elems[Offset])) {
      return ElemTy;
    }
  }
  return nullptr;
}

static llvm::DIType *getVarTypeFromIRRec(const llvm::Value *V, size_t Depth) {
  static constexpr size_t DepthLimit = 10;
  V = V->stripPointerCastsAndAliases();

  if (auto *VarTy = getVarTypeFromIRImpl(V)) {
    return VarTy;
  }

  const auto InternalGetOffsetAndBase =
      [](const llvm::Value *Val) -> std::pair<const llvm::Value *, size_t> {
    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Val)) {
      return getOffsetAndBase(Load->getPointerOperand());
    }
    if (const auto *Gep = llvm::dyn_cast<llvm::GEPOperator>(Val)) {
      return getOffsetAndBase(Gep->getPointerOperand());
    }
    return {};
  };

  auto [Base, Offset] = InternalGetOffsetAndBase(V);
  if (!Base || Depth >= DepthLimit) {
    return nullptr;
  }

  auto *BaseTy = getVarTypeFromIRRec(Base, Depth + 1);
  if (!BaseTy) {
    return nullptr;
  }
  return getStructElementType(BaseTy, Offset);
}

llvm::DIType *getVarTypeFromIR(const llvm::Value *V) {
  if (!V) {
    return nullptr;
  }
  return getVarTypeFromIRRec(V, 0);
}

const llvm::DIType *stripPointerTypes(const llvm::DIType *DITy) {
  while (const auto *DerivedTy =
             llvm::dyn_cast_or_null<llvm::DIDerivedType>(DITy)) {
    if (DerivedTy->getTag() == llvm::dwarf::DW_TAG_pointer_type ||
        DerivedTy->getTag() == llvm::dwarf::DW_TAG_reference_type ||
        DerivedTy->getTag() == llvm::dwarf::DW_TAG_rvalue_reference_type ||
        DerivedTy->getTag() == llvm::dwarf::DW_TAG_typedef ||
        DerivedTy->getTag() == llvm::dwarf::DW_TAG_const_type) {
      DITy = DerivedTy->getBaseType();
    } else {
      break;
    }
  }
  return DITy;
}

const llvm::DIType *getReceiverType(const llvm::CallBase *CallSite) {
  if (!CallSite || CallSite->arg_empty() ||
      (CallSite->hasStructRetAttr() && CallSite->arg_size() < 2)) {
    return nullptr;
  }

  const auto *Receiver =
      CallSite->getArgOperand(unsigned(CallSite->hasStructRetAttr()));

  if (!Receiver->getType()->isPointerTy()) {
    return nullptr;
  }

  if (const auto *DITy = getVarTypeFromIR(Receiver)) {
    return stripPointerTypes(DITy);
  }

  if (const auto *Var =
          getDILocalVariable(Receiver->stripPointerCastsAndAliases())) {
    return stripPointerTypes(Var->getType());
  }

  return nullptr;
}

std::string getReceiverTypeName(const llvm::CallBase *CallSite) {
  const auto *RT = getReceiverType(CallSite);
  if (RT) {
    return RT->getName().str();
  }
  return "";
}

static bool isAddressTakenImpl(const llvm::Value *F) {
  if (!F) {
    return false;
  }

  for (const auto &Use : F->uses()) {
    const auto *User = Use.getUser();

    if (llvm::isa<llvm::GlobalAlias>(User)) {
      if (isAddressTakenImpl(User)) {
        return true;
      }
      continue;
    }

    if (const auto *Glob = llvm::dyn_cast<llvm::GlobalVariable>(User)) {
      if (Glob->getName() == "llvm.compiler.used" ||
          Glob->getName() == "llvm.used") {
        continue;
      }
      return true;
    }

    const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
    if (!Call) {
      return true;
    }

    if (Call->isDebugOrPseudoInst()) {
      continue;
    }

    const auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(Call);
    if (Intrinsic && Intrinsic->isAssumeLikeIntrinsic()) {
      continue;
    }

    if (Call->isCallee(&Use)) {
      continue;
    }

    return true;
  }

  return false;
}

bool isAddressTakenFunction(const llvm::Function *F) {
  return isAddressTakenImpl(F);
}

bool isVirtualCall(const llvm::Instruction *Inst,
                   const LLVMVFTableProvider &VTP) {
  if (!Inst) {
    return false;
  }
  const auto *CallSite = llvm::dyn_cast<llvm::CallBase>(Inst);
  if (!CallSite) {
    return false;
  }
  const auto *RecType = getReceiverType(CallSite);
  if (!RecType) {
    return false;
  }

  if (!VTP.hasVFTable(RecType)) {
    return false;
  }
  auto Idx = getVFTIndex(CallSite);
  return Idx.has_value();
}

bool isHeapAllocatingFunction(const llvm::Function *Fun) noexcept {
  if (!Fun) {
    return false;
  }
  auto FunName = Fun->getName();
  if (FunName == "realloc") {
    return true;
  }
  return isHeapAllocatingFunction(FunName);
}

bool isHeapAllocatingFunction(llvm::StringRef FunName) noexcept {
  static const llvm::DenseSet<llvm::StringRef> HeapAllocatingFunctions = {
      "malloc", "calloc", "realloc", "valloc", "alloca",
      "_Znwm", "_Znam", "_Znwj", "_Znaj",
      "_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t",
      "_ZnwjRKSt9nothrow_t", "_ZnajRKSt9nothrow_t",
  };
  return HeapAllocatingFunctions.contains(FunName);
}

const llvm::Function *walkConstInitPath(const llvm::Constant *Init,
                                        llvm::ArrayRef<uint64_t> Indices) {
  if (!Init) {
    return nullptr;
  }
  if (Indices.empty()) {
    return llvm::dyn_cast<llvm::Function>(Init->stripPointerCastsAndAliases());
  }
  const uint64_t Idx0 = Indices[0];
  const llvm::Constant *Elem = nullptr;
  if (const auto *CA = llvm::dyn_cast<llvm::ConstantArray>(Init)) {
    if (Idx0 >= CA->getNumOperands()) {
      return nullptr;
    }
    Elem = CA->getOperand(Idx0);
  } else if (llvm::isa<llvm::ConstantStruct>(Init)) {
    if (Idx0 != 0) {
      return nullptr;
    }
    Elem = Init;
  } else {
    return nullptr;
  }
  for (const uint64_t Idx : Indices.drop_front(1)) {
    const auto *Agg = llvm::dyn_cast<llvm::ConstantAggregate>(Elem);
    if (!Agg || Idx >= Agg->getNumOperands()) {
      return nullptr;
    }
    Elem = Agg->getOperand(Idx);
  }
  return llvm::dyn_cast_or_null<llvm::Function>(
      Elem->stripPointerCastsAndAliases());
}

} // namespace lotus
