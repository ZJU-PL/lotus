#include "Analysis/CallGraph/VTA/TypeAssignmentGraph.h"
#include "Analysis/CallGraph/Resolver.h"
#include "Analysis/CallGraph/VirtualCallUtils.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

namespace lotus::vta {

static void printNodeImpl(llvm::raw_ostream &OS, Variable Var) {
  OS << "var-";
  if (Var.Val) {
    if (Var.Val->hasName()) {
      OS << Var.Val->getName();
    } else {
      OS << *Var.Val;
    }
  }
}

static void printNodeImpl(llvm::raw_ostream &OS, Field Fld) {
  OS << "fld-";
  if (Fld.Base) {
    OS << Fld.Base->getName();
  }
  OS << '+' << Fld.ByteOffset;
}

static void printNodeImpl(llvm::raw_ostream &OS, Return Ret) {
  OS << "ret-";
  if (Ret.Fun) {
    OS << Ret.Fun->getName();
  }
}

void printNode(llvm::raw_ostream &OS, const TAGNode &TN) {
  std::visit([&OS](auto Nod) { printNodeImpl(OS, Nod); }, TN.Label);
}

void TypeAssignmentGraph::print(llvm::raw_ostream &OS) const {
  OS << "digraph TAG {\n";
  for (size_t Ctr = 0; Ctr < Nodes.size(); ++Ctr) {
    OS << "  " << Ctr << " [label=\"";
    printNode(OS, Nodes[Ctr]);
    OS << "\"];\n";
  }
  for (size_t Ctr = 0; Ctr < Adj.size(); ++Ctr) {
    for (auto Tgt : Adj[Ctr]) {
      OS << "  " << Ctr << " -> " << Tgt << ";\n";
    }
  }
  OS << "}\n";
}

static void addFields(const llvm::Module &M, TypeAssignmentGraph &TAG,
                      const llvm::DataLayout &DL) {
  size_t PointerSize = DL.getPointerSize();
  llvm::DebugInfoFinder DIF;
  DIF.processModule(M);

  for (const auto *DITy : DIF.types()) {
    if (const auto *CompTy = llvm::dyn_cast_or_null<llvm::DICompositeType>(DITy)) {
      if (CompTy->getTag() == llvm::dwarf::DW_TAG_structure_type ||
          CompTy->getTag() == llvm::dwarf::DW_TAG_class_type) {
        size_t Offset = 0;
        if (auto Elements = CompTy->getElements()) {
          for (const auto *Elem : Elements) {
            if (const auto *Derived = llvm::dyn_cast_or_null<llvm::DIDerivedType>(Elem)) {
              Offset = Derived->getOffsetInBits() / 8;
              TAG.getOrInsert({Field{CompTy, Offset}});
            }
          }
        }
        TAG.getOrInsert({Field{CompTy, SIZE_MAX}});
      }
    }
  }
}

static void addGlobals(const llvm::Module &M, TypeAssignmentGraph &TAG) {
  for (const auto &Glob : M.globals()) {
    if (Glob.getValueType()->isIntOrIntVectorTy() ||
        Glob.getValueType()->isFloatingPointTy()) {
      continue;
    }
    auto GlobName = Glob.getName();
    if (GlobName.startswith("_ZTV") || GlobName.startswith("_ZTI") ||
        GlobName.startswith("_ZTS")) {
      continue;
    }
    TAG.getOrInsert({Variable{&Glob}});
  }
}

static void initializeWithFun(const llvm::Function *Fun, TypeAssignmentGraph &TAG) {
  for (const auto &Param : Fun->args()) {
    TAG.getOrInsert({Variable{&Param}});
  }
  TAG.getOrInsert({Return{Fun}});

  for (const auto &I : llvm::instructions(Fun)) {
    if (!I.getType()->isVoidTy()) {
      TAG.getOrInsert({Variable{&I}});
    }
  }
}

static std::optional<TAGNodeId>
getGEPNode(const llvm::GetElementPtrInst *GEP, TypeAssignmentGraph &TAG,
           const llvm::DataLayout &DL) {
  auto Offs = [&]() -> size_t {
    llvm::APInt Offs(64, 0);
    if (GEP->accumulateConstantOffset(DL, Offs)) {
      return Offs.getZExtValue();
    }
    return SIZE_MAX;
  }();

  auto *VarTy = getVarTypeFromIR(GEP);
  if (!VarTy) {
    return std::nullopt;
  }

  if (auto Direct = TAG.get({Field{VarTy, Offs}})) {
    return Direct;
  }
  return TAG.get({Field{VarTy, SIZE_MAX}});
}

static void handleAlloca(const llvm::AllocaInst *Alloca, TypeAssignmentGraph &TAG,
                         const LLVMVFTableProvider &VTP) {
  auto TNId = TAG.get({Variable{Alloca}});
  if (!TNId) {
    return;
  }

  const auto *VarTy = getVarTypeFromIR(Alloca);
  if (!VarTy) {
    return;
  }
  const auto *CompTy = llvm::dyn_cast_or_null<llvm::DICompositeType>(stripPointerTypes(VarTy));
  if (CompTy && VTP.hasVFTable(CompTy)) {
    TAG.TypeEntryPoints[*TNId].insert(CompTy);
  }
}

static void handleLoad(const llvm::LoadInst *Load, TypeAssignmentGraph &TAG,
                       const llvm::DataLayout &DL) {
  auto To = TAG.get({Variable{Load}});
  if (!To) {
    return;
  }

  auto From = TAG.get({Variable{Load->getPointerOperand()}});
  if (From) {
    TAG.addEdge(*From, *To);
  }

  if (const auto *GEP =
          llvm::dyn_cast<llvm::GetElementPtrInst>(Load->getPointerOperand())) {
    if (auto GEPNodeId = getGEPNode(GEP, TAG, DL)) {
      TAG.addEdge(*GEPNodeId, *To);
    }
  }
}

static void handleGEP(const llvm::GetElementPtrInst *GEP,
                      TypeAssignmentGraph &TAG, const llvm::DataLayout &DL) {
  auto To = TAG.get({Variable{GEP}});
  if (!To) {
    return;
  }
  auto From = TAG.get({Variable{GEP->getPointerOperand()}});
  if (From) {
    TAG.addEdge(*From, *To);
  }

  if (auto GEPNodeId = getGEPNode(GEP, TAG, DL)) {
    TAG.addEdge(*GEPNodeId, *To);
  }
}

static void handleStore(const llvm::StoreInst *Store, TypeAssignmentGraph &TAG,
                        const llvm::DataLayout &DL) {
  const auto *BaseFn = llvm::dyn_cast_or_null<llvm::Function>(
      Store->getValueOperand()->stripPointerCastsAndAliases());

  if (const auto *GEPDest =
          llvm::dyn_cast<llvm::GetElementPtrInst>(Store->getPointerOperand())) {
    if (auto GEPNodeId = getGEPNode(GEPDest, TAG, DL)) {
      if (BaseFn) {
        TAG.TypeEntryPoints[*GEPNodeId].insert(BaseFn);
      } else {
        auto From = TAG.get({Variable{Store->getValueOperand()}});
        if (From) {
          TAG.addEdge(*From, *GEPNodeId);
        }
      }
    }
  }

  auto DestId = TAG.get({Variable{Store->getPointerOperand()}});
  if (DestId) {
    if (BaseFn) {
      TAG.TypeEntryPoints[*DestId].insert(BaseFn);
    } else {
      auto From = TAG.get({Variable{Store->getValueOperand()}});
      if (From) {
        TAG.addEdge(*From, *DestId);
      }
    }
  }
}

static void handlePhi(const llvm::PHINode *Phi, TypeAssignmentGraph &TAG) {
  auto To = TAG.get({Variable{Phi}});
  if (!To) {
    return;
  }
  for (const auto &Inc : Phi->incoming_values()) {
    auto From = TAG.get({Variable{Inc.get()}});
    if (From) {
      TAG.addEdge(*From, *To);
    }
  }
}

static void handleCall(const llvm::CallBase *Call, TypeAssignmentGraph &TAG,
                       Resolver &BaseRes, const LLVMVFTableProvider &VTP) {
  auto CSNod = TAG.get({Variable{Call}});

  if (const auto *MDNode = Call->getMetadata("heapallocsite")) {
    if (const auto *CompTy = llvm::dyn_cast_or_null<llvm::DICompositeType>(MDNode)) {
      if (CSNod && (CompTy->getTag() == llvm::dwarf::DW_TAG_structure_type ||
                    CompTy->getTag() == llvm::dwarf::DW_TAG_class_type)) {
        TAG.TypeEntryPoints[*CSNod].insert(CompTy);
      }
    }
  }

  llvm::SmallVector<std::optional<TAGNodeId>, 4> Args;
  llvm::SmallVector<bool, 4> EntryArgs;

  for (const auto &Arg : Call->args()) {
    auto TN = TAG.get({Variable{Arg.get()}});
    Args.push_back(TN);
    bool IsEntry =
        llvm::isa<llvm::Function>(Arg.get()->stripPointerCastsAndAliases());
    EntryArgs.push_back(IsEntry);
  }

  const auto HandleCallTarget = [&](const llvm::Function *Callee) {
    if (Callee->isDeclaration()) {
      return;
    }

    for (const auto &[Param, Arg] : llvm::zip(Callee->args(), Args)) {
      auto ParamNodId = TAG.get({Variable{&Param}});
      if (!ParamNodId) {
        continue;
      }

      if (Param.getArgNo() < EntryArgs.size() && EntryArgs[Param.getArgNo()]) {
        if (const auto *FnArg = llvm::dyn_cast<llvm::Function>(
                Call->getArgOperand(Param.getArgNo())->stripPointerCastsAndAliases())) {
          TAG.TypeEntryPoints[*ParamNodId].insert(FnArg);
        }
      }

      if (Arg.has_value()) {
        TAG.addEdge(*Arg, *ParamNodId);
      }
    }

    if (CSNod) {
      auto RetNod = TAG.get({Return{Callee}});
      if (RetNod) {
        TAG.addEdge(*RetNod, *CSNod);
      }
    }
  };

  if (const auto *StaticCallee = llvm::dyn_cast_or_null<llvm::Function>(
          Call->getCalledOperand()->stripPointerCastsAndAliases())) {
    HandleCallTarget(StaticCallee);
  } else {
    for (const auto *Callee : BaseRes.resolveIndirectCall(Call)) {
      HandleCallTarget(Callee);
    }
  }
}

static void handleReturn(const llvm::ReturnInst *Ret,
                         TypeAssignmentGraph &TAG) {
  auto TNId = TAG.get({Return{Ret->getFunction()}});
  if (!TNId) {
    return;
  }

  if (const auto *RetVal = Ret->getReturnValue()) {
    const auto *Base = RetVal->stripPointerCastsAndAliases();
    if (const auto *RetFun = llvm::dyn_cast<llvm::Function>(Base)) {
      TAG.TypeEntryPoints[*TNId].insert(RetFun);
      return;
    }

    auto From = TAG.get({Variable{Base}});
    if (From) {
      TAG.addEdge(*From, *TNId);
    }
  }
}

TypeAssignmentGraph computeTypeAssignmentGraph(
    const llvm::Module &M, const LLVMVFTableProvider &VTP, Resolver &BaseRes) {
  TypeAssignmentGraph TAG;
  const auto &DL = M.getDataLayout();

  addFields(M, TAG, DL);
  addGlobals(M, TAG);

  for (const auto &F : M) {
    if (!F.isDeclaration()) {
      initializeWithFun(&F, TAG);
    }
  }

  for (const auto &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (const auto &I : llvm::instructions(F)) {
      if (llvm::isa<llvm::DbgInfoIntrinsic>(&I)) {
        continue;
      }
      if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
        handleAlloca(Alloca, TAG, VTP);
      } else if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        handleLoad(Load, TAG, DL);
      } else if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
        handleGEP(GEP, TAG, DL);
      } else if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        handleStore(Store, TAG, DL);
      } else if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
        handlePhi(Phi, TAG);
      } else if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
        auto From = TAG.get({Variable{Cast->getOperand(0)}});
        auto To = TAG.get({Variable{Cast}});
        if (From && To) {
          TAG.addEdge(*From, *To);
        }
      } else if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
        handleCall(Call, TAG, BaseRes, VTP);
      } else if (const auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
        handleReturn(Ret, TAG);
      }
    }
  }

  return TAG;
}

} // namespace lotus::vta
