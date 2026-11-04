/**
 * @file FLTAPass.h
 * @brief First Layer Type Analysis pass for FPA
 *
 * This file provides the FLTAPass class for performing first-layer
 * type analysis in the FPA framework. It implements fuzzy type matching
 * to identify potential callee functions for indirect calls based on
 * function signature compatibility.
 *
 * @ingroup FPA
 */

//
// Created by prophe cheng on 2025/4/1.
//

#pragma once

#include "Analysis/CallGraph/FPA/CallGraphPass.h"

class FLTAPass : public CallGraphPass {
public:
  DenseMap<size_t, FuncSet> MatchedFuncsMap;
  DenseMap<size_t, FuncSet> MatchedICallTypeMap;

  FLTAPass(GlobalContext *Ctx_) : CallGraphPass(Ctx_) {
    ID = "first layer type analysis";
  }

  bool fuzzyTypeMatch(Type *Ty1, Type *Ty2, Module *M1, Module *M2);

  void findCalleesWithType(CallInst *, FuncSet &);

  void analyzeIndCall(CallInst *CI, FuncSet *FS) override;

  bool doInitialization(Module *M) override;
};

