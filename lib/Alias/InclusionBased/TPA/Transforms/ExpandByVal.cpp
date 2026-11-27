/**
 * @file ExpandByVal.cpp
 * @brief Expand out use of "byval" and "sret" attributes.
 *
 * This pass expands out by-value passing of structs as arguments and
 * return values. In LLVM IR terms, it expands out "byval" and "sret"
 * function argument attributes.
 *
 * NOTE: This is a simplified version for LLVM 14 migration.
 * Byval arguments are copied into a callee-local slot before the
 * attribute is dropped, and by-value semantics are preserved.
 *
 * @author rainoftime
 */
#include "Alias/InclusionBased/TPA/Transforms/ExpandByVal.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

#include <vector>

using namespace llvm;

namespace transform {

/**
 * @brief Run the ExpandByValPass on a module.
 *
 * Removes byval and sret attributes from all functions in the module.
 * This simplifies the IR by eliminating these special calling conventions.
 *
 * @param M The module to transform
 * @param analysisManager Module analysis manager (unused)
 * @return PreservedAnalyses::none() if modified, PreservedAnalyses::all()
 * otherwise
 */
PreservedAnalyses ExpandByValPass::run(Module &M,
                                       ModuleAnalysisManager &analysisManager) {
  bool modified = false;

  for (auto &func : M) {
    // Leave external declarations alone: their ABI must match the libraries
    // they are linked against, and rewriting their call sites would break it.
    if (func.isDeclaration())
      continue;

    auto attrs = func.getAttributes();
    bool funcModified = false;

    // Remove byval and sret from return attributes
    auto retAttrs = attrs.getRetAttrs();
    if (retAttrs.hasAttribute(Attribute::ByVal) ||
        retAttrs.hasAttribute(Attribute::StructRet)) {
      retAttrs = retAttrs.removeAttribute(func.getContext(), Attribute::ByVal)
                     .removeAttribute(func.getContext(), Attribute::StructRet);
      funcModified = true;
    }

    // Remove byval and sret from function attributes
    auto fnAttrs = attrs.getFnAttrs();
    if (fnAttrs.hasAttribute(Attribute::ByVal) ||
        fnAttrs.hasAttribute(Attribute::StructRet)) {
      fnAttrs = fnAttrs.removeAttribute(func.getContext(), Attribute::ByVal)
                    .removeAttribute(func.getContext(), Attribute::StructRet);
      funcModified = true;
    }

    // Remove byval and sret from all argument attributes
    SmallVector<AttributeSet, 8> newArgAttrs;
    for (unsigned argIdx = 0; argIdx < func.arg_size(); ++argIdx) {
      auto argAttrs = attrs.getParamAttrs(argIdx);
      if (argAttrs.hasAttribute(Attribute::ByVal) ||
          argAttrs.hasAttribute(Attribute::StructRet)) {
        // Handle the actual copying of byval arguments: copy the incoming
        // object into a callee-local slot and redirect all uses to it, so
        // dropping the attribute does not make the callee alias the caller.
        if (argAttrs.hasAttribute(Attribute::ByVal)) {
          Argument *Arg = func.getArg(argIdx);
          Type *byValTy = argAttrs.getByValType();
          if (!byValTy && Arg->getType()->isPointerTy())
            byValTy = Arg->getType()->getPointerElementType();
          if (byValTy && byValTy->isSized()) {
            Module *mod = func.getParent();
            LLVMContext &ctx = func.getContext();
            Instruction *insertPt =
                &*func.getEntryBlock().getFirstInsertionPt();

            auto *slot =
                new AllocaInst(byValTy, 0, Arg->getName() + ".byval", insertPt);
            Arg->replaceAllUsesWith(slot);

            Type *i8PtrDst = Type::getInt8PtrTy(
                ctx, slot->getType()->getPointerAddressSpace());
            Type *i8PtrSrc = Type::getInt8PtrTy(
                ctx, Arg->getType()->getPointerAddressSpace());
            Value *dstPtr = slot->getType() == i8PtrDst
                                ? static_cast<Value *>(slot)
                                : CastInst::CreatePointerCast(
                                      slot, i8PtrDst, "byval.dst", insertPt);
            Value *srcPtr = Arg->getType() == i8PtrSrc
                                ? static_cast<Value *>(Arg)
                                : CastInst::CreatePointerCast(
                                      Arg, i8PtrSrc, "byval.src", insertPt);

            Type *sizeTy = IntegerType::get(ctx, 64);
            Type *tys[3] = {i8PtrDst, i8PtrSrc, sizeTy};
            Function *memcpyFn =
                Intrinsic::getDeclaration(mod, Intrinsic::memcpy, tys);
            Value *args[4] = {
                dstPtr, srcPtr,
                ConstantInt::get(
                    sizeTy, mod->getDataLayout().getTypeAllocSize(byValTy)),
                ConstantInt::getFalse(ctx)};
            auto *call = CallInst::Create(memcpyFn->getFunctionType(), memcpyFn,
                                          args, "", insertPt);
            auto location = insertPt->getDebugLoc();
            if (!location)
              if (auto *scope = func.getSubprogram())
                location =
                    DILocation::get(ctx, scope->getScopeLine(), 0, scope);
            call->setDebugLoc(location);
          }
        }
        argAttrs =
            argAttrs.removeAttribute(func.getContext(), Attribute::ByVal)
                .removeAttribute(func.getContext(), Attribute::StructRet);
        funcModified = true;
      }
      newArgAttrs.push_back(argAttrs);
    }

    if (funcModified) {
      auto newAttrs =
          AttributeList::get(func.getContext(), fnAttrs, retAttrs,
                             llvm::ArrayRef<llvm::AttributeSet>(newArgAttrs));
      func.setAttributes(newAttrs);
      modified = true;

      // Keep in-module call sites consistent with the rewritten signature.
      for (User *U : func.users())
        if (auto *CB = dyn_cast<CallBase>(U)) {
          if (CB->getCalledOperand()->stripPointerCasts() != &func)
            continue;
          for (unsigned argIdx = 0;
               argIdx < CB->arg_size() && argIdx < func.arg_size();
               ++argIdx) {
            CB->removeParamAttr(argIdx, Attribute::ByVal);
            CB->removeParamAttr(argIdx, Attribute::StructRet);
          }
        }
    }
  }

  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform
