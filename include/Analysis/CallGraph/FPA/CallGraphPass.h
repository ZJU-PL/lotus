/**
 * @file CallGraphPass.h
 * @brief Base class for call graph passes in FPA analysis
 *
 * This file provides the CallGraphPass base class for function pointer
 * analysis in the FPA (Function Pointer Analysis) framework. It handles
 * module iteration, indirect call analysis, and type-based call resolution.
 *
 * @ingroup FPA
 */

//
// Created by prophe cheng on 2024/1/3.
//

#pragma once

#include "Analysis/CallGraph/FPA/Common.h"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

// 主要关注3个函数，doInitialization、doFinalization、doModulePass
// doInitialization完成type-hierachy分析以及type-propagation
// doFinalization将结果dump出
// doModulePass进行间接调用分析
class CallGraphPass {
public:
  const char *ID;
  GlobalContext *Ctx;
  // General pointer types like char * and void *
  map<Module *, Type *> Int8PtrTy;
  // long interger type
  map<Module *, Type *> IntPtrTy;
  map<Module *, const DataLayout *> DLMap;
  set<CallInst *> CallSet;
  set<CallInst *> ICallSet;
  set<CallInst *> VCallSet;
  set<CallInst *> MatchedICallSet;

  size_t MIdx;

  CallGraphPass(GlobalContext *Ctx_, const char *ID_ = NULL)
      : ID(ID_), Ctx(Ctx_), MIdx(0) {}

  // Run on each module before iterative pass.
  virtual bool doInitialization(Module *M);

  // Run on each module after iterative pass.
  virtual bool doFinalization(Module *M);

  // Iterative pass.
  virtual bool doModulePass(Module *M);

  virtual void analyzeIndCall(CallInst *callInst, FuncSet *FS) = 0;

  void run(ModuleList &modules);

  void intersectFuncSets(FuncSet &FS1, FuncSet &FS2, FuncSet &FS);

  void unrollLoops(Function *F);

  bool isVirtualCall(CallInst *CI);

  bool isVirtualFunction(Function *F);
};

