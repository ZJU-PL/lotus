/**
 * @file Annotation.h
 * @brief Annotation utilities for TypeQualifier analysis
 *
 * This file provides utility functions for working with LLVM metadata
 * annotations used in the TypeQualifier analysis framework. It includes
 * functions for extracting annotation information, generating scope names for
 * values, and identifying allocation functions.
 *
 * @ingroup TypeQualifier
 */

//
// Created by ubuntu on 12/26/17.
//

#ifndef UBIANALYSIS_ANNOTATION_H
#define UBIANALYSIS_ANNOTATION_H

#include <cstddef>
#pragma once

#include <string>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>

using namespace llvm;
#define MD_ID "id"
#define MD_NAME_INFO_OP 0
#define MD_ARG_INFO_OP 1
// #define WARNING(stmt) (errs()<<stmt);

static inline bool isFunctionPointer(llvm::Type *Ty) {
  if (!Ty->isPointerTy())
    return false;
  return Ty->getPointerElementType()->isFunctionTy();
}

static inline bool isFunctionPointerOrVoid(llvm::Type *Ty) {
  if (!Ty->isPointerTy())
    return false;

  llvm::Type *subTy = Ty->getPointerElementType();
  if (subTy->isFunctionTy())
    return true;

  // FIXME should add something in clang to indicate real void*
  // like in CPI
  if (llvm::IntegerType *ITy = llvm::dyn_cast<llvm::IntegerType>(subTy))
    if (ITy->getBitWidth() == 8)
      return true;

  return false;
}

inline StringRef getStringFromMD(MDNode *MD, unsigned index = MD_NAME_INFO_OP) {
  if (MDString *S = dyn_cast_or_null<MDString>(MD->getOperand(index)))
    return S->getString();
  return "";
}

static inline std::string getScopeName(const llvm::GlobalValue *GV) {
  if (llvm::GlobalValue::isExternalLinkage(GV->getLinkage()))
    return GV->getName().str();
  else {
    llvm::StringRef moduleName =
        llvm::sys::path::stem(GV->getParent()->getModuleIdentifier());
    return "_" + moduleName.str() + "." + GV->getName().str();
  }
}

static inline std::string getScopeName(llvm::Instruction *I) {
  llvm::Function *F = I->getParent()->getParent();
  assert(F && "Cannot get Function");
  llvm::StringRef moduleName =
      llvm::sys::path::stem(F->getParent()->getModuleIdentifier());
  if (I->hasName())
    return "_" + moduleName.str() + "." + F->getName().str() + "." +
           I->getName().str();

  return "_" + moduleName.str() + "." + F->getName().str() + ".anonymous";
}

// prefix anonymous struct name with module name
static inline std::string getScopeName(const llvm::StructType *Ty,
                                       const llvm::Module *M) {
  //	errs()<<"Inside getScopeName:\n";
  if (Ty->isLiteral())
    return "";
  llvm::StringRef structName = Ty->getStructName();
  size_t dotPos = structName.rfind('.');
#ifdef DBG
  if (M)
    errs() << "Module is found.\n";
  else
    errs() << "Module Not Found.\n";
#endif
  if (M && (structName.startswith("struct.anon") ||
            structName.startswith("union.anon"))) {
    llvm::StringRef rest = structName.substr(6);
    llvm::StringRef moduleName =
        llvm::sys::path::stem(M->getModuleIdentifier());
    return "struct._" + moduleName.str() + rest.str();
  }
  if (dotPos != 0 && dotPos != StringRef::npos && structName.back() != '.' &&
      isdigit(static_cast<unsigned char>(structName[dotPos + 1])))
    structName = structName.substr(0, dotPos);
  return structName.str();
}
static inline std::string getScopeName(const llvm::Function *Func) {
  const llvm::Module *M = Func->getParent();
  // llvm::StringRef moduleName = llvm::sys::path::stem(
  // M->getModuleIdentifier());
  std::string moduleName = M->getName().str();
  for (size_t i = 0; i < moduleName.size(); i++) {
    if (moduleName[i] == '/')
      moduleName[i] = '_';
  }
  std::string FName = Func->getName().str();
  // Special case: make the names of syscalls consistent
  if (FName.find("SyS_") == 0)
    FName = "sys_" + FName.substr(4);
  return moduleName + "_" + FName;
}
static inline llvm::StringRef getLoadStoreId(llvm::Instruction *I) {
  if (llvm::MDNode *MD = I->getMetadata(MD_ID)) {
    return getStringFromMD(MD);
  }
  return llvm::StringRef();
}

extern bool isAllocFn(llvm::StringRef name, int *size, int *flag);
static inline bool isAllocFn(llvm::StringRef name) {
  int size, flag;
  return isAllocFn(name, &size, &flag);
}

static inline std::string getVarId(const llvm::GlobalValue *GV) {
  if (!GV) {
    return "";
  }
  return "var." + getScopeName(GV);
}

static inline std::string getVarId(llvm::AllocaInst *AI) {
  return "lvar." + getScopeName(AI);
}
static inline std::string getArgId(llvm::Function *F, unsigned no) {
  return "arg." + getScopeName(F) + "." + llvm::Twine(no).str();
}

static inline std::string getArgId(llvm::Argument *A) {
  return getArgId(A->getParent(), A->getArgNo());
}
static inline std::string getStructId(llvm::StructType *STy, llvm::Module *M,
                                      unsigned offset) {
  if (!STy || STy->isLiteral())
    return "";
  return getScopeName(STy, M) + "," + llvm::Twine(offset).str();
}

static inline std::string getStructId(Type *Ty, Module *M) {
  if (Ty->isPointerTy()) {
    Ty = Ty->getContainedType(0);
    if (StructType *STy = dyn_cast<StructType>(Ty))
      return getScopeName(STy, M);
  }
  return "";
}
static inline std::string getRetId(llvm::Function *F) {
  return "ret." + getScopeName(F);
}

static inline std::string getValueId(llvm::Value *V);
static inline std::string getRetId(llvm::CallInst *CI) {
  if (llvm::Function *CF = CI->getCalledFunction())
    return getRetId(CF);
  else {
    std::string sID = getValueId(CI->getCalledOperand());
    if (sID != "")
      return "ret." + sID;
  }
  return "";
}
static inline std::string getValueId(llvm::Value *V) {
  if (llvm::Argument *A = llvm::dyn_cast<llvm::Argument>(V))
    return getArgId(A);
  else if (llvm::CallInst *CI = llvm::dyn_cast<llvm::CallInst>(V)) {
    if (llvm::Function *F = CI->getCalledFunction())
      if (F->getName().startswith("kint_arg.i"))
        return getLoadStoreId(CI).str();
    return getRetId(CI);
  } else if (llvm::isa<llvm::LoadInst>(V) || llvm::isa<llvm::StoreInst>(V)) {
    return getLoadStoreId(llvm::dyn_cast<llvm::Instruction>(V)).str();
  } else if (llvm::isa<llvm::AllocaInst>(V)) {
    return getVarId(dyn_cast<llvm::AllocaInst>(V));
  }

  if (Instruction *I = dyn_cast<Instruction>(V)) {
    return "unknown." + getScopeName(I);
  }

  errs() << "No Value Id for : " << *V << "\n";

  return "";
}

extern std::string getAnnotation(llvm::Value *V, llvm::Module *M);
extern std::string getLoadId(llvm::LoadInst *LI);
extern std::string getStoreId(llvm::StoreInst *SI);
extern std::string getAnonStructId(llvm::Value *V, llvm::Module *M,
                                   llvm::StringRef Prefix);
extern std::string getStructId(llvm::Value *PV, llvm::User::op_iterator &IS,
                               llvm::User::op_iterator &IE, llvm::Module *M);

#endif // UBIANALYSIS_ANNOTATION_H
