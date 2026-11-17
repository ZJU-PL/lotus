#include "Checker/TPA/Engine/Initializer.h"

#include "Alias/InclusionBased/TPA/Context/Context.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Checker/TPA/Engine/TaintGlobalState.h"
#include "Checker/TPA/Program/DefUseModule.h"
#include "Checker/TPA/Support/TaintEnv.h"
#include "Checker/TPA/Support/TaintMemo.h"

#include <utility>

#include <llvm/IR/Function.h>

using namespace context;

namespace tpa::taint {

Initializer::Initializer(TaintGlobalState &g, TaintMemo &m)
    : duModule(g.getDefUseModule()), env(g.getEnv()),
      ptrAnalysis(g.getPointerAnalysis()), memo(m) {}

void Initializer::initializeMainArgs(TaintStore &store) {
  auto const &entryFunc = duModule.getEntryFunction().getFunction();
  const auto *globalCtx = Context::getGlobalContext();

  if (entryFunc.arg_size() > 0) {
    // argc is tainted
    const auto *argcValue = entryFunc.arg_begin();
    env.strongUpdate(TaintValue(globalCtx, &*argcValue), TaintLattice::Tainted);

    if (entryFunc.arg_size() > 1) {
      // argv is not tainted
      auto *argvValue = entryFunc.getArg(1);
      env.strongUpdate(TaintValue(globalCtx, argvValue),
                       TaintLattice::Untainted);

      // *argv and **argv are tainted
      const auto *argvObj = ptrAnalysis.getMemoryManager().getArgvObject();
      store.strongUpdate(argvObj, TaintLattice::Tainted);

      if (entryFunc.arg_size() > 2) {
        auto *envpValue = entryFunc.getArg(2);
        env.strongUpdate(TaintValue(globalCtx, envpValue),
                         TaintLattice::Untainted);

        const auto *envpObj = ptrAnalysis.getMemoryManager().getEnvpObject();
        store.strongUpdate(envpObj, TaintLattice::Tainted);
      }
    }
  }
}

void Initializer::initializeGlobalVariables(TaintStore &store) {
  const auto *globalCtx = Context::getGlobalContext();
  for (auto const &global : duModule.getModule().globals()) {
    // We don't need to worry about global variables in env because all of them
    // are constants and are already handled properly

    auto pSet = ptrAnalysis.getPtsSet(globalCtx, &global);
    for (const auto *obj : pSet) {
      if (!obj->isSpecialObject())
        for (const auto *field :
             ptrAnalysis.getMemoryManager().getReachableMemoryObjects(obj))
          store.strongUpdate(field, TaintLattice::Untainted);
    }
  }
  store.strongUpdate(tpa::MemoryManager::getUniversalObject(),
                     TaintLattice::Either);
}

WorkList Initializer::runOnInitState(TaintStore &&initStore) {
  WorkList workList;

  initializeMainArgs(initStore);
  initializeGlobalVariables(initStore);

  const auto *entryCtx = context::Context::getGlobalContext();
  const auto *entryInst = duModule.getEntryFunction().getEntryInst();
  assert(entryInst != nullptr);

  auto pp = ProgramPoint(entryCtx, entryInst);
  memo.update(pp, std::move(initStore));
  workList.enqueue(pp);

  return workList;
}

} // namespace tpa::taint