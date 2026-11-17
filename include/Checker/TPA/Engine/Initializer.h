#pragma once

#include "Checker/TPA/Engine/WorkList.h"
#include "Checker/TPA/Support/TaintStore.h"

namespace tpa {
class SemiSparsePointerAnalysis;
} // namespace tpa

namespace tpa::taint {

class DefUseModule;
class TaintEnv;
class TaintGlobalState;
class TaintMemo;

class Initializer {
private:
  const DefUseModule &duModule;
  TaintEnv &env;
  const tpa::SemiSparsePointerAnalysis &ptrAnalysis;
  TaintMemo &memo;

  void initializeMainArgs(TaintStore &);
  void initializeGlobalVariables(TaintStore &);

public:
  Initializer(TaintGlobalState &g, TaintMemo &m);

  WorkList runOnInitState(TaintStore &&initStore);
};

} // namespace tpa::taint