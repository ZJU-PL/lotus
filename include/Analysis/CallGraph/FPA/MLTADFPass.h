/**
 * @file MLTADFPass.h
 * @brief Data Flow Enhanced Multi-Layer Type Analysis pass
 *
 * This file provides the MLTADFPass class for enhanced multi-layer type
 * analysis with data flow information. It extends MLTAPass with data flow
 * tracking to improve function pointer escape analysis and type confinement.
 *
 * @ingroup FPA
 */

//
// Created by prophe cheng on 2025/4/4.
//

#pragma once

#include "Analysis/CallGraph/FPA/Config.h"
#include "Analysis/CallGraph/FPA/MLTAPass.h"

class MLTADFPass : public MLTAPass {
private:
  set<Instruction *> nonEscapeStores;

public:
  MLTADFPass(GlobalContext *Ctx_) : MLTAPass(Ctx_) {
    ID = "data flow enhanced multi layer type analysis";
  }

  void typeConfineInStore(StoreInst *SI) override;

  void escapeFuncPointer(Value *PO, Instruction *I) override;

  // resolve simple function pointer: v = f(a1, ...).
  // I: v = f(...), V: f, callees: potential targets, return value: whether this
  // is simple indirect-call The last argument is used to process recursive call
  bool resolveSFP(Value *User, Value *V, set<Function *> &callees,
                  set<Value *> &defUseSites, set<Function *> &visitedFuncs);

  bool justifyUsers(Value *value, Value *curUser);
};

