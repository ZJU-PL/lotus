#include "Analysis/CallGraph/OTF/OTFResolver.h"
#include "Analysis/CallGraph/RTA/RTAResolver.h"
#include "Analysis/CallGraph/VirtualCallUtils.h"
#include "Analysis/TypeHierarchy/DIBasedTypeHierarchy.h"
#include "Analysis/TypeHierarchy/LLVMVFTable.h"
#include "Analysis/TypeHierarchy/LLVMVFTableProvider.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

namespace lotus {

namespace {

std::vector<std::pair<const llvm::Value *, const llvm::Value *>>
getActualFormalPointerPairs(const llvm::CallBase *CallSite,
                            const llvm::Function *CalleeTarget) {
  std::vector<std::pair<const llvm::Value *, const llvm::Value *>> Pairs;
  Pairs.reserve(CallSite->arg_size());

  unsigned Idx = 0;
  for (; Idx < CallSite->arg_size() && Idx < CalleeTarget->arg_size(); ++Idx) {
    if (CallSite->getArgOperand(Idx)->getType()->isPointerTy() &&
        CalleeTarget->getArg(Idx)->getType()->isPointerTy()) {
      Pairs.emplace_back(CallSite->getArgOperand(Idx),
                         CalleeTarget->getArg(Idx));
    }
  }

  return Pairs;
}

} // namespace

OTFResolver::OTFResolver(const llvm::Module *M,
                         const LLVMVFTableProvider *VTP,
                         const DIBasedTypeHierarchy *TH,
                         AliasProviderFn ExternalAliasProvider)
    : Resolver(M, VTP), TH(TH),
      ExternalAliasProvider(std::move(ExternalAliasProvider)) {
  if (M && VTP) {
    FallbackRTA = std::make_unique<RTAResolver>(M, VTP, TH);
  }
  initializeIntraProceduralAliases();
}

OTFResolver::~OTFResolver() = default;

void OTFResolver::introduceAlias(const llvm::Value *V1, const llvm::Value *V2) {
  if (!V1 || !V2) {
    return;
  }
  const auto *S1 = V1->stripPointerCastsAndAliases();
  const auto *S2 = V2->stripPointerCastsAndAliases();
  AliasGraph[V1].insert(V2);
  AliasGraph[V2].insert(V1);
  AliasGraph[S1].insert(S2);
  AliasGraph[S2].insert(S1);
  AliasGraph[V1].insert(S2);
  AliasGraph[V2].insert(S1);
}

void OTFResolver::initializeIntraProceduralAliases() {
  if (!M) {
    return;
  }

  for (const auto &F : *M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (const auto &I : llvm::instructions(F)) {
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        const auto *Val = Store->getValueOperand();
        const auto *Ptr = Store->getPointerOperand();
        if (Val->getType()->isPointerTy()) {
          introduceAlias(Val, Ptr);
        }
      } else if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        if (Load->getType()->isPointerTy()) {
          introduceAlias(&I, Load->getPointerOperand());
        }
      } else if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
        if (Cast->getType()->isPointerTy() &&
            Cast->getOperand(0)->getType()->isPointerTy()) {
          introduceAlias(&I, Cast->getOperand(0));
        }
      } else if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
        if (Phi->getType()->isPointerTy()) {
          for (const auto &Inc : Phi->incoming_values()) {
            introduceAlias(&I, Inc.get());
          }
        }
      } else if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
        if (Select->getType()->isPointerTy()) {
          introduceAlias(&I, Select->getTrueValue());
          introduceAlias(&I, Select->getFalseValue());
        }
      } else if (const auto *GEP =
                     llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
        introduceAlias(&I, GEP->getPointerOperand());
      }
    }
  }
}

OTFResolver::AliasSetTy
OTFResolver::getAliasSet(const llvm::Value *V,
                         const llvm::Instruction *At) const {
  AliasSetTy Result;
  if (!V) {
    return Result;
  }

  llvm::SmallVector<const llvm::Value *, 16> Worklist;
  auto Enqueue = [&](const llvm::Value *Node) {
    if (!Node) {
      return;
    }
    if (Result.insert(Node).second) {
      Worklist.push_back(Node);
    }
    const auto *Stripped = Node->stripPointerCastsAndAliases();
    if (Result.insert(Stripped).second) {
      Worklist.push_back(Stripped);
    }
  };

  Enqueue(V);

  if (ExternalAliasProvider) {
    for (const auto *ExtAlias : ExternalAliasProvider(V, At)) {
      Enqueue(ExtAlias);
    }
  }

  while (!Worklist.empty()) {
    const auto *Curr = Worklist.pop_back_val();
    auto It = AliasGraph.find(Curr);
    if (It != AliasGraph.end()) {
      for (const auto *Neighbor : It->second) {
        Enqueue(Neighbor);
      }
    }
  }

  return Result;
}

void OTFResolver::handlePossibleTargets(const llvm::CallBase *CallSite,
                                        FunctionSetTy &CalleeTargets) {
  for (const auto *CalleeTarget : CalleeTargets) {
    if (!CalleeTarget || CalleeTarget->isDeclaration()) {
      continue;
    }

    for (auto &[Actual, Formal] :
         getActualFormalPointerPairs(CallSite, CalleeTarget)) {
      introduceAlias(Actual, Formal);
    }

    if (CalleeTarget->getReturnType()->isPointerTy()) {
      for (const auto &I : llvm::instructions(CalleeTarget)) {
        if (const auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
          if (const auto *RetVal = Ret->getReturnValue()) {
            introduceAlias(CallSite, RetVal);
          }
        }
      }
    }
  }
}

void OTFResolver::resolveVirtualCall(FunctionSetTy &PossibleTargets,
                                     const llvm::CallBase *CallSite) {
  auto RetrievedVtableIndex = getVFTIndexAndVT(CallSite);
  if (!RetrievedVtableIndex.has_value()) {
    if (FallbackRTA) {
      FallbackRTA->resolveVirtualCall(PossibleTargets, CallSite);
    }
    return;
  }

  auto [VtablePtr, VtableIndex] = RetrievedVtableIndex.value();
  auto PTS = getAliasSet(VtablePtr, CallSite);

  for (const auto *P : PTS) {
    if (const auto *PGV = llvm::dyn_cast<llvm::GlobalVariable>(P)) {
      if (PGV->hasName() &&
          PGV->getName().startswith(DIBasedTypeHierarchy::VTablePrefix) &&
          PGV->hasInitializer()) {
        if (const auto *PCS =
                llvm::dyn_cast<llvm::ConstantStruct>(PGV->getInitializer())) {
          auto VFs = LLVMVFTable::getVFVectorFromIRVTable(*PCS);
          if (VtableIndex >= VFs.size()) {
            continue;
          }
          const auto *Callee = VFs[VtableIndex];
          if (!Callee || !Callee->hasName() ||
              Callee->getName() == DIBasedTypeHierarchy::PureVirtualCallName ||
              !isConsistentCall(CallSite, Callee)) {
            continue;
          }
          PossibleTargets.insert(Callee);
        }
      }
    }
  }

  if (PossibleTargets.empty() && FallbackRTA) {
    FallbackRTA->resolveVirtualCall(PossibleTargets, CallSite);
  }
}

void OTFResolver::resolveFunctionPointer(FunctionSetTy &PossibleTargets,
                                         const llvm::CallBase *CallSite) {
  if (!CallSite->getCalledOperand()) {
    return;
  }

  if (auto StructVCall = getStructVCallInfo(CallSite)) {
    const auto &[BasePtr, Indices, SourceElemTy] = *StructVCall;
    auto BaseAliases = getAliasSet(BasePtr, CallSite);

    for (const auto *BaseAlias : BaseAliases) {
      if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(BaseAlias)) {
        if (GV->hasInitializer()) {
          if (const auto *TargetFn =
                  walkConstInitPath(GV->getInitializer(), Indices)) {
            if (isConsistentCall(CallSite, TargetFn)) {
              PossibleTargets.insert(TargetFn);
            }
          }
        }
      }
    }

    if (M) {
      for (const auto &Glob : M->globals()) {
        if (Glob.hasInitializer() && Glob.getValueType() == SourceElemTy) {
          if (const auto *TargetFn =
                  walkConstInitPath(Glob.getInitializer(), Indices)) {
            if (isConsistentCall(CallSite, TargetFn)) {
              PossibleTargets.insert(TargetFn);
            }
          }
        }
      }

      for (const auto &F : *M) {
        if (F.isDeclaration()) {
          continue;
        }
        for (const auto &I : llvm::instructions(F)) {
          const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
          if (!Store) {
            continue;
          }
          const auto *StoredFn = llvm::dyn_cast<llvm::Function>(
              Store->getValueOperand()->stripPointerCastsAndAliases());
          if (!StoredFn || !isConsistentCall(CallSite, StoredFn)) {
            continue;
          }
          if (auto ConstAccess =
                  getConstGEPFieldAccess(Store->getPointerOperand())) {
            const auto &[StoreBase, StoreIndices, StoreElemTy] = *ConstAccess;
            if (StoreIndices == Indices && StoreElemTy == SourceElemTy) {
              PossibleTargets.insert(StoredFn);
            }
          }
        }
      }
    }
  }

  auto PTS = getAliasSet(CallSite->getCalledOperand(), CallSite);

  llvm::SmallVector<const llvm::GlobalVariable *, 2> GlobalVariableWL;
  llvm::SmallVector<const llvm::ConstantAggregate *, 4> ConstantAggregateWL;
  llvm::SmallPtrSet<const llvm::ConstantAggregate *, 4>
      VisitedConstantAggregates;

  for (const auto *P : PTS) {
    if (!llvm::isa<llvm::Constant>(P)) {
      continue;
    }

    GlobalVariableWL.clear();
    ConstantAggregateWL.clear();

    if (const auto *F = llvm::dyn_cast<llvm::Function>(P)) {
      if (isConsistentCall(CallSite, F)) {
        PossibleTargets.insert(F);
      }
    }

    if (const auto *GVP = llvm::dyn_cast<llvm::GlobalVariable>(P)) {
      GlobalVariableWL.push_back(GVP);
    } else if (const auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(P)) {
      for (const auto &Op : CE->operands()) {
        if (const auto *GVOp = llvm::dyn_cast<llvm::GlobalVariable>(Op)) {
          GlobalVariableWL.push_back(GVOp);
        }
      }
    }

    if (GlobalVariableWL.empty()) {
      continue;
    }

    for (const auto *GV : GlobalVariableWL) {
      if (!GV->hasInitializer()) {
        continue;
      }
      const auto *InitConst = GV->getInitializer();
      if (const auto *InitConstAggregate =
              llvm::dyn_cast<llvm::ConstantAggregate>(InitConst)) {
        ConstantAggregateWL.push_back(InitConstAggregate);
      }
    }

    VisitedConstantAggregates.clear();

    while (!ConstantAggregateWL.empty()) {
      const auto *ConstAggregateItem = ConstantAggregateWL.pop_back_val();
      if (!VisitedConstantAggregates.insert(ConstAggregateItem).second) {
        continue;
      }
      for (const auto &Op : ConstAggregateItem->operands()) {
        if (const auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(Op)) {
          if (CE->getType()->isPointerTy() && CE->isCast()) {
            if (const auto *F =
                    llvm::dyn_cast<llvm::Function>(CE->getOperand(0));
                F && isConsistentCall(CallSite, F)) {
              PossibleTargets.insert(F);
            }
          }
        }

        if (const auto *F = llvm::dyn_cast<llvm::Function>(Op)) {
          if (isConsistentCall(CallSite, F)) {
            PossibleTargets.insert(F);
          }
        } else if (auto *CA = llvm::dyn_cast<llvm::ConstantAggregate>(Op)) {
          ConstantAggregateWL.push_back(CA);
        } else if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Op)) {
          if (GV->hasInitializer()) {
            if (auto *GVCA = llvm::dyn_cast<llvm::ConstantAggregate>(
                    GV->getInitializer())) {
              ConstantAggregateWL.push_back(GVCA);
            }
          }
        }
      }
    }
  }

  if (PossibleTargets.empty()) {
    Resolver::resolveFunctionPointer(PossibleTargets, CallSite);
  }
}

} // namespace lotus
