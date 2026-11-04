/**
 * @file KELPPass.h
 * @brief KELP (Knowledge-Enhanced Learning-based Pointer) analysis pass
 *
 * This file provides the KELPPass class for enhanced function pointer
 * analysis using knowledge-enhanced techniques. It extends MLTADFPass
 * with additional heuristics for handling simple indirect calls and
 * confined address-taken functions.
 *
 * @ingroup FPA
 */

//
// Created by prophe cheng on 2025/4/4.
//

#pragma once

#include "Analysis/CallGraph/FPA/MLTADFPass.h"

class KELPPass : public MLTADFPass {
private:
  set<CallInst *> simpleIndCalls;
  set<Function *> confinedAddrTakenFuncs;

public:
  KELPPass(GlobalContext *Ctx_) : MLTADFPass(Ctx_) { ID = "kelp analysis"; }

  bool doInitialization(Module *) override;

  bool doFinalization(Module *M) override;

  void analyzeIndCall(CallInst *CI, FuncSet *FS) override;
};

