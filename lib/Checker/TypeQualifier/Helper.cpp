#include "Checker/TypeQualifier/Helper.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"

#include <iostream>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>
#include <stdio.h>
#include <unistd.h>

using namespace llvm;
std::string StrFN[] = {
    "strcmp",   "strncmp",    "strcpy",    "strlwr",  "strcat",
    "strlen",   "strupr",     "strrchr",   "strncat", "strsep",
    "snprintf", "vscnprintf", "vsnprintf", "",
};
std::set<std::string> StrFuncs(StrFN, StrFN + 9);

bool getRealType(llvm::Value *V, llvm::CallInst *CI, Type *T1, Type *T2,
                 std::set<llvm::Function *> &);
bool checkCastType(Type *T1, Type *T2, llvm::Function *F, int argNo,
                   std::set<llvm::Function *> &);
int64_t getGEPOffset(const Value *value, const DataLayout *dataLayout) {
  // Assume this function always receives GEP value
  const GEPOperator *gepValue = dyn_cast<GEPOperator>(value);
  assert(gepValue != NULL && "getGEPOffset receives a non-gep value!");
  assert(dataLayout != nullptr && "getGEPOffset receives a NULL dataLayout!");

  int64_t offset = 0;
  const Value *baseValue = gepValue->getPointerOperand()->stripPointerCasts();
  // If we have yet another nested GEP const expr, accumulate its offset
  // The reason why we don't accumulate nested GEP instruction's offset is
  // that we aren't required to. Also, it is impossible to do that because
  // we are not sure if the indices of a GEP instruction contains all-consts or
  // not.
  if (const ConstantExpr *cexp = dyn_cast<ConstantExpr>(baseValue))
    if (cexp->getOpcode() == Instruction::GetElementPtr) {
      offset += getGEPOffset(cexp, dataLayout);
    }
  Type *ptrTy = gepValue->getSourceElementType();

  SmallVector<Value *, 4> indexOps(gepValue->op_begin() + 1,
                                   gepValue->op_end());
  // Make sure all indices are constants
  for (unsigned i = 0, e = indexOps.size(); i != e; ++i) {
    if (!isa<ConstantInt>(indexOps[i]))
      indexOps[i] = ConstantInt::get(Type::getInt32Ty(ptrTy->getContext()), 0);
  }
  offset += dataLayout->getIndexedOffsetInType(ptrTy, indexOps);
  return offset;
}

unsigned offsetToFieldNum(const Value *ptr, int64_t offset,
                          const DataLayout *dataLayout,
                          const StructAnalyzer *structAnalyzer,
                          Module *module) {
  assert(ptr->getType()->isPointerTy() &&
         "Passing a non-ptr to offsetToFieldNum!");
  assert(dataLayout != nullptr &&
         "DataLayout is NULL when calling offsetToFieldNum!");
  if (offset < 0)
    return 0;
  Type *trueElemType = cast<PointerType>(ptr->getType())->getPointerElementType();
  unsigned ret = 0;
  if (trueElemType->isStructTy()) {
    StructType *stType = cast<StructType>(trueElemType);
    if (!stType->isLiteral() && stType->getName().startswith("union"))
      return ret;
    if (!stType->isLiteral() && stType->getName().startswith("union"))
      return ret;
    if (stType->isOpaque())
      return ret;
  }
  while (offset > 0) {
    // Collapse array type
    while (const ArrayType *arrayType = dyn_cast<ArrayType>(trueElemType))
      trueElemType = arrayType->getPointerElementType();

    if (trueElemType->isStructTy()) {
      StructType *stType = cast<StructType>(trueElemType);

      const StructInfo *stInfo = structAnalyzer->getStructInfo(stType, module);
      if (stInfo == NULL) {
        errs() << "Warning: struct info is missed, correctness is not "
                  "guanranteed here.\n";
        break;
      }
      stType = const_cast<StructType *>(stInfo->getRealType());

      const StructLayout *stLayout =
          stInfo->getDataLayout()->getStructLayout(stType);
      uint64_t allocSize = stInfo->getDataLayout()->getTypeAllocSize(stType);
      if (!allocSize)
        return 0;
      offset %= allocSize;
      unsigned idx = stLayout->getElementContainingOffset(offset);
      if (!stType->isLiteral() && stType->getName().startswith("union")) {
        offset -= stInfo->getDataLayout()->getTypeAllocSize(stType);
        if (offset <= 0)
          break;
      }
      ret += stInfo->getOffset(idx);

      if (!stType->isLiteral() && stType->getName().startswith("union")) {
        offset -= stInfo->getDataLayout()->getTypeAllocSize(stType);
      } else {
        offset -= stLayout->getElementOffset(idx);
      }
      trueElemType = stType->getElementType(idx);
    } else {
      //  errs()<<"dataLayout->getTypeAllocSize(trueElemType) =
      //  "<<dataLayout->getTypeAllocSize(trueElemType)<<"\n";
      offset %= dataLayout->getTypeAllocSize(trueElemType);
      if (offset != 0) {
        errs() << "Warning: GEP into the middle of a field. This usually "
                  "occurs when union is used. Since partial alias is not "
                  "supported, correctness is not guanranteed here.\n";
        break;
      }
    }
  }
  return ret;
}

std::vector<std::string> strSplit(std::string &str, const std::string pattern) {
  std::vector<std::string> ret;
  if (str == "")
    return ret;
  std::string strs = str + pattern;
  size_t pos = strs.find(pattern);
  while (pos != strs.npos) {
    std::string temp = strs.substr(0, pos);
    ret.push_back(temp);
    strs = strs.substr(pos + 1, strs.size());
    pos = strs.find(pattern);
  }
  return ret;
}

#define VAR_MATCH_VAR (false)
bool isCompatibleType(Type *T1, Type *T2, llvm::Function *func, int argNo) {
  if (T1->isPointerTy()) {
    if (!T2->isPointerTy())
      return false;

    Type *ElT1 = cast<PointerType>(T1)->getElementType();
    Type *ElT2 = cast<PointerType>(T2)->getElementType();

    // assume "void *" and "char *" are equivalent to any pointer type
    if (ElT1->isIntegerTy(8) /*|| ElT2->isIntegerTy(8)*/) {
      if (func && func->hasName())
        errs() << "Candidate function :" << func->getName().str() << "\n";
      // if (const StructType *stType = dyn_cast<StructType>(ElT1))
      //	errs()<<"ElT1 from candidate
      //name:"<<stType->getName().str()<<"\n";
      //  errs()<<"ElT1 from candidate "<<*ElT1<<"\n";
      // errs()<<"ElT2 from caller: "<<*ElT2<<"\n";
      // return true;
      std::set<llvm::Function *> visit;
      return checkCastType(T1, T2, func, argNo, visit);
    }

    return isCompatibleType(ElT1, ElT2);
  } else if (T1->isArrayTy()) {
    if (!T2->isArrayTy())
      return false;

    Type *ElT1 = T1->getArrayElementType();
    Type *ElT2 = T2->getArrayElementType();

    if (ElT1->isIntegerTy(8) /*|| ElT2->isIntegerTy(8)*/) {
      if (func && func->hasName())
        errs() << "Array Candidate function :" << func->getName().str() << "\n";
      // if (const StructType *stType = dyn_cast<StructType>(ElT1))
      //      errs()<<"ElT1 from candidate
      //      name:"<<stType->getName().str()<<"\n";
      //  errs()<<"ElT1 from candidate "<<*ElT1<<"\n";
      // errs()<<"ElT2 from caller: "<<*ElT2<<"\n";
      // return true;
      std::set<llvm::Function *> visit;
      return checkCastType(T1, T2, func, argNo, visit);
    }

    return isCompatibleType(ElT1, ElT1);
  } else if (T1->isIntegerTy()) {
    // assume pointer can be cased to the address space size
    if (T2->isPointerTy() &&
        T1->getIntegerBitWidth() == T2->getPointerAddressSpace())
      return true;

    // assume all integer type are compatible
    if (T2->isIntegerTy())
      return true;
    else
      return false;
  } else if (T1->isStructTy()) {
    StructType *ST1 = cast<StructType>(T1);
    StructType *ST2 = dyn_cast<StructType>(T2);
    if (!ST2)
      return false;

    // literal has to be equal
    if (ST1->isLiteral() != ST2->isLiteral())
      return false;

    // literal, compare content
    if (ST1->isLiteral()) {
      unsigned numEl1 = ST1->getNumElements();
      if (numEl1 != ST2->getNumElements())
        return false;

      for (unsigned i = 0; i < numEl1; ++i) {
        if (!isCompatibleType(ST1->getElementType(i), ST2->getElementType(i)))
          return false;
      }
      return true;
    }

    // not literal, use name?
    return ST1->getStructName().equals(ST2->getStructName());
  } else if (T1->isFunctionTy()) {
    FunctionType *FT1 = cast<FunctionType>(T1);
    FunctionType *FT2 = dyn_cast<FunctionType>(T2);
    if (!FT2)
      return false;

    if (!isCompatibleType(FT1->getReturnType(), FT2->getReturnType()))
      return false;

    // assume varg is always compatible with varg?
    if (FT1->isVarArg()) {
      if (FT2->isVarArg())
        return VAR_MATCH_VAR;
      else
        return !VAR_MATCH_VAR;
    }

    // compare args, again ...
    unsigned numParam1 = FT1->getNumParams();
    if (numParam1 != FT2->getNumParams())
      return false;

    for (unsigned i = 0; i < numParam1; ++i) {
      if (!isCompatibleType(FT1->getParamType(i), FT2->getParamType(i)))
        return false;
    }
    return true;
  } else {
    errs() << "Unhandled Types:" << *T1 << " :: " << *T2 << "\n";
    return T1->getTypeID() == T2->getTypeID();
  }
}
bool checkCastType(Type *T1, Type *T2, llvm::Function *F, int argNo,
                   std::set<llvm::Function *> &Visit) {
  if (!F || argNo < 0 || Visit.find(F) != Visit.end())
    return true;
  Visit.insert(F);
  errs() << "checkCastType for callee:  " << F->getName().str()
         << ", argNo = " << argNo << "\n";

  int count = 0;
  llvm::Value *val = NULL;
  for (Argument &A : F->args()) {
    if (argNo == count) {
      for (User *U : A.users()) {
        if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
          llvm::Value *argaddr = SI->getOperand(1);
          for (User *Use : argaddr->users()) {
            if (LoadInst *LI = dyn_cast<LoadInst>(Use)) {
              for (User *LIUse : LI->users()) {
                if (BitCastInst *BI = dyn_cast<BitCastInst>(LIUse)) {
                  // Use BI to get the real type
                  if (cast<PointerType>(BI->getType())->getElementType() !=
                      cast<PointerType>(T1)->getElementType()) {
                    errs() << "Not Compatible.\n";
                    return false;
                  }
                } // BI
                  // check CI, if the T1 (from callee) is the string while the
                  // T2 (from caller) is a struct then it's  not compatible
                if (CallInst *CI = dyn_cast<CallInst>(LIUse)) {
                  if (CI->getCalledFunction()) {
                    Type *ElT2 = cast<PointerType>(T2)->getElementType();
                    if (const StructType *stType = dyn_cast<StructType>(ElT2)) {
                      std::string fName =
                          CI->getCalledFunction()->getName().str();
                      if (StrFuncs.count(fName)) {
                        errs() << "Not Compatible.\n";
                        return false;
                      }
                    }
                    if (!getRealType(LI, CI, T1, T2, Visit)) {
                      return false;
                    }
                  } // CI->getCalledFunction
                } // CallInst *CI = dyn_cast<CallInst>(LIUse)
              }
            }
          }
        }
      }
      break;
    } else {
      count++;
      if (count > argNo)
        break;
    }
  }
  return true;
}
bool getRealType(llvm::Value *V, llvm::CallInst *CI, Type *T1, Type *T2,
                 std::set<llvm::Function *> &Visit) {
  // 1.identify the argNo for V in CI
  int argNo = -1;
  llvm::Function *Callee = CI->getCalledFunction();
  for (int i = 0; i < (int)CI->arg_size(); i++) {
    if (CI->getArgOperand(i) == V) {
      argNo = i;
      break;
    }
  }
  if (argNo < 0) {
    return true;
  }
  return checkCastType(T1, T2, Callee, argNo, Visit);
}
std::string getCurrentWorkingDir(void) {
  char buff[FILENAME_MAX];
  getcwd(buff, FILENAME_MAX);
  std::string current_working_dir(buff);
  return current_working_dir;
}
