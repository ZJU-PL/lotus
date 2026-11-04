/**
 * @file InsertGlobalCtorCallPass.cpp
 * @brief Insert calls to global constructors at the beginning of main.
 *
 * This pass processes the @llvm.global_ctors array and inserts calls to
 * all constructor functions at the beginning of the main function (or
 * cr_main). This ensures global constructors are executed before main,
 * making the IR more explicit for pointer analysis.
 *
 * @author peiming
 */
#include "Alias/InclusionBased/AserPTA/PreProcessing/Passes/InsertGlobalCtorCallPass.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

using namespace aser;
using namespace llvm;

#define INIT_FUNC_INDEX 1

/**
 * @brief Run the InsertGlobalCtorCallPass on a module.
 *
 * Extracts constructor functions from @llvm.global_ctors and inserts calls
 * to them at the beginning of the main function (or cr_main).
 *
 * @param M The module to process
 * @return false (this pass doesn't modify the module structure, only adds
 * calls)
 */
bool InsertGlobalCtorCallPass::runOnModule(llvm::Module &M) {
  auto *ctors = M.getGlobalVariable("llvm.global_ctors");
  if (ctors == nullptr) {
    // no global ctors
    return false;
  }
  // TODO: make main configurable
  auto *mainFun = M.getFunction("cr_main");
  if (mainFun == nullptr || mainFun->isDeclaration()) {
    return false;
  }

  IRBuilder<> builder(&mainFun->getEntryBlock().front());

  // @llvm.global_ctors = [N x { i32, void ()*, i8* }]
  if (ctors->hasInitializer()) {
    const llvm::Constant *initializer = ctors->getInitializer();
    if (initializer->isNullValue() ||
        llvm::isa<llvm::UndefValue>(initializer)) {
      return false;
    }

    // traverse the init array
    auto *initArray = llvm::cast<llvm::ConstantArray>(initializer);
    for (unsigned i = 0; i < initArray->getNumOperands(); i++) {
      llvm::Constant *curCtor = initArray->getOperand(i);
      // the ctor is a structure of type { i32, void ()*, i8* }
      llvm::Constant *init =
          llvm::cast<llvm::ConstantAggregate>(curCtor)->getOperand(1);
      auto *initFun = llvm::cast<Function>(init);
      builder.CreateCall(FunctionCallee(initFun->getFunctionType(), initFun));
    }
  }
  return false;
}

char InsertGlobalCtorCallPass::ID = 0;
static RegisterPass<InsertGlobalCtorCallPass>
    IGCCP("", "Insert call to global variable constructor before main",
          true, /*CFG only*/
          false /*is analysis*/);
