/// \file Analyzer.h
/// FiTx CFG-based typestate analyzer: path-insensitive, inter-procedural,
/// return-code aware state propagation (paper Section 4.2, 4.3).
#pragma once
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Module.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Core/ValueTypeAlias.h"
#include "Checker/FiTx/Frontend/BasicBlock.h"
#include "Checker/FiTx/Frontend/Function.h"
#include "Checker/FiTx/Frontend/State.h"
#include "Checker/FiTx/Frontend/StateTransition.h"
#include "Checker/FiTx/Frontend/Utils.h"

#include <map>
#include <memory>
#include <set>
#include <stack>
#include <vector>

namespace fitx {

class Analyzer {
public:
  Analyzer(llvm::Module &llvm_module, fitx::StateManager &state_manager,
           fitx::LoggingClient &client);

  void analyze();

  void analyzeFunction(std::shared_ptr<fitx::Function> F);
  void analyzeCallInst(std::shared_ptr<fitx::Instruction> I);
  void analyzeStoreInst(std::shared_ptr<fitx::Instruction> I);
  void analyzeLoadInst(std::shared_ptr<fitx::Instruction> I);
  bool analyzeFunctionCall(std::shared_ptr<fitx::CallInst> call_inst);
  void analyzeReturnValue(std::shared_ptr<fitx::Function> function);

  void analyzePrevBlockBranch(std::shared_ptr<fitx::BasicBlock> B);

  void changeValueState(llvm::ArrayRef<Transition> transitions,
                        const std::shared_ptr<fitx::Value> &value,
                        const std::shared_ptr<fitx::Instruction> &inst);

  void generateError(
      BugNotificationTiming timing,
      const std::set<std::shared_ptr<fitx::Value>, BasicBlock::ValuePtrCompare>
          *values = nullptr);

  bool functionInformationExists(std::shared_ptr<fitx::Function> function);
  void copyFunctionValues(std::shared_ptr<fitx::Function> called_func,
                          std::shared_ptr<fitx::CallInst> call_inst);

  bool addPendingFunctionValues(std::shared_ptr<fitx::Function> called_func,
                                std::shared_ptr<fitx::CallInst> call_inst);

  std::shared_ptr<FunctionInformation> currentFunctionInformation() {
    return function_info_[analyzing_function_.top()];
  }

  std::shared_ptr<FunctionInformation>
  getFunctionInformation(std::shared_ptr<fitx::Function> function);

  void checkAlias(std::shared_ptr<fitx::StoreInst> store_inst);

private:
  llvm::Module &llvm_module_;
  fitx::StateManager &state_manager_;
  fitx::LoggingClient &log_;

  std::stack<std::shared_ptr<fitx::Function>> analyzing_function_;

  std::map<std::shared_ptr<fitx::Function>,
           std::shared_ptr<FunctionInformation>>
      function_info_;

  std::shared_ptr<fitx::BasicBlockInformation> bb_info_;
};
} // namespace fitx
