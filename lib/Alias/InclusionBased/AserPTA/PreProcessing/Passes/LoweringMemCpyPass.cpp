/**
 * @file LoweringMemCpyPass.cpp
 * @brief Lower memcpy/memmove calls into explicit load/store operations.
 *
 * This pass replaces memcpy and memmove function calls with explicit GEP,
 * load, and store operations. This makes memory operations explicit in the
 * IR, which helps pointer analysis track memory flows more precisely.
 *
 * @author peiming
 */
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"

#include "Alias/InclusionBased/AserPTA/Util/Log.h"

#include <array>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/NoFolder.h>

using namespace std;
using namespace aser;
using namespace llvm;

/**
 * @brief Recursively lower memcpy for a specific type.
 *
 * Recursively processes struct and array types to generate GEP, load, and
 * store operations that copy the data. For pointer, scalar and vector types,
 * generates a single load/store of the whole value.
 *
 * @param type The type to lower memcpy for
 * @param src Source pointer value
 * @param dst Destination pointer value
 * @param idx Current GEP indices (modified during recursion)
 * @param builder IRBuilder for creating instructions
 */
void LoweringMemCpyPass::lowerMemCpyForType(Type *type, Value *src, Value *dst,
                                            SmallVector<Value *, 5> &idx,
                                            IRBuilder<NoFolder> &builder) {
  switch (type->getTypeID()) {
  case llvm::Type::StructTyID: {
    const auto *structType = static_cast<const StructType *>(type);
    for (unsigned i = 0; i < structType->getNumElements(); i++) {
      idx.push_back(ConstantInt::get(idxType, i));
      lowerMemCpyForType(structType->getElementType(i), src, dst, idx, builder);
      idx.pop_back();
    }
    break;
  }
  case llvm::Type::ArrayTyID: {
    const auto *arrayType = static_cast<const ArrayType *>(type);
    for (unsigned i = 0; i < arrayType->getNumElements(); i++) {
      idx.push_back(ConstantInt::get(idxType, i));
      lowerMemCpyForType(arrayType->getElementType(), src, dst, idx, builder);
      idx.pop_back();
    }
    break;
  }
  case llvm::Type::FixedVectorTyID:
  case llvm::Type::ScalableVectorTyID:
  default: {
    // pointer, scalar or vector field: copy it as a single whole value.
    auto *srcGEP = builder.CreateGEP(src->getType()->getPointerElementType(),
                                     src, idx, "");
    auto *dstGEP = builder.CreateGEP(dst->getType()->getPointerElementType(),
                                     dst, idx, "");

    auto *srcLoad =
        builder.CreateLoad(srcGEP->getType()->getPointerElementType(), srcGEP);
    builder.CreateStore(srcLoad, dstGEP, false);
    break;
  }
  }
}

bool LoweringMemCpyPass::runOnModule(llvm::Module &M) {
  // LOG_DEBUG(); // TODO add progress message?
  if (idxType == nullptr) {
    // use i32 to index getelementptr
    // TODO: does it matter to use i32 instead of i64?
    idxType = IntegerType::get(M.getContext(), 32);
  }

  bool changed = false;
  auto &DL = M.getDataLayout();
  IRBuilder<NoFolder> builder(M.getContext());

  array<StringRef, 2> MemCpys{"llvm.memcpy.p0i8.p0i8.i32",
                              "llvm.memcpy.p0i8.p0i8.i64"};
  for (StringRef MemCpyName : MemCpys) {
    Function *memcpy = M.getFunction(MemCpyName);
    if (memcpy != nullptr) {
      for (auto *user : memcpy->users()) {
        if (auto *callInst = dyn_cast<CallInst>(user)) {
          Value *dst = callInst->getArgOperand(0);
          Value *src = callInst->getArgOperand(1);
          Value *len = callInst->getArgOperand(2);

          auto *constLen = dyn_cast<ConstantInt>(len);
          auto *dstBitCast = dyn_cast<BitCastInst>(dst);
          auto *srcBitCast = dyn_cast<BitCastInst>(src);

          if (constLen && dstBitCast && srcBitCast) {
            // we only lowering memcpy that uses
            // 1, constant length
            // 2, We can infer the type of dst and source, and theirs is the
            // same
            Type *dstType = dstBitCast->getSrcTy();
            Type *srcType = srcBitCast->getSrcTy();

            if (dstType == srcType) {
              Type *elemType = dstType->getPointerElementType();
              // The allocation size is unsigned; a negative constant length can
              // never match it, so compare as unsigned without a sign mismatch.
              if (DL.getTypeAllocSize(elemType) ==
                  static_cast<uint64_t>(constLen->getSExtValue())) {
                changed = true;
                builder.SetInsertPoint(callInst);

                SmallVector<Value *, 5> idx;
                idx.push_back(ConstantInt::get(idxType, 0));

                lowerMemCpyForType(elemType, srcBitCast->getOperand(0),
                                   dstBitCast->getOperand(0), idx, builder);

                // do some simple cleanups
                callInst->eraseFromParent();
                if (srcBitCast->getNumUses() == 0) {
                  srcBitCast->eraseFromParent();
                }
                if (dstBitCast->getNumUses() == 0 &&
                    dstBitCast->getParent() !=
                        nullptr) { // src might be equal to dst
                  dstBitCast->eraseFromParent();
                }
              }
            }
          }
        }
      }
    }
  }
  return changed;
}

char aser::LoweringMemCpyPass::ID = 0;
static RegisterPass<aser::LoweringMemCpyPass> LMCPY("", "Lowering MemCpy call",
                                                    false, /*CFG only*/
                                                    false /*is analysis*/);
