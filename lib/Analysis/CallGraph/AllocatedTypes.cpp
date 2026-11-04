#include "Analysis/CallGraph/AllocatedTypes.h"
#include "Analysis/CallGraph/VirtualCallUtils.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"

namespace lotus {

static const llvm::DICompositeType *
isCompositeStructType(const llvm::DIType *Ty) {
  if (const auto *CompTy = llvm::dyn_cast_or_null<llvm::DICompositeType>(Ty);
      CompTy && (CompTy->getTag() == llvm::dwarf::DW_TAG_structure_type ||
                 CompTy->getTag() == llvm::dwarf::DW_TAG_class_type)) {
    return CompTy;
  }
  return nullptr;
}

std::vector<const llvm::DICompositeType *>
collectAllocatedTypes(const llvm::Module &Mod) {
  llvm::DenseSet<const llvm::DICompositeType *> AllocatedTypes;

  for (const auto &Fun : Mod) {
    for (const auto &Inst : llvm::instructions(Fun)) {
      if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&Inst)) {
        if (const auto *Ty = isCompositeStructType(getVarTypeFromIR(Alloca))) {
          AllocatedTypes.insert(Ty);
        }
      } else if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
        if (const auto *Callee = llvm::dyn_cast<llvm::Function>(
                Call->getCalledOperand()->stripPointerCastsAndAliases())) {
          if (isHeapAllocatingFunction(Callee)) {
            const auto *MDNode = Call->getMetadata("heapallocsite");
            if (const auto *CompTy =
                    llvm::dyn_cast_or_null<llvm::DICompositeType>(MDNode);
                isCompositeStructType(CompTy)) {
              AllocatedTypes.insert(CompTy);
            }
          }
        }
      }
    }
  }

  std::vector<const llvm::DICompositeType *> Result;
  Result.reserve(AllocatedTypes.size());
  Result.insert(Result.end(), AllocatedTypes.begin(), AllocatedTypes.end());
  return Result;
}

} // namespace lotus
