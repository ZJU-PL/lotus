/** @file TaintModel.h @brief Declarative taint source, transfer, and sink specifications. */
#pragma once

#include "SymbolicExecution/Domain/ProgramVar.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace SymbolicExecution {

/// Declarative taint specification consulted by the symbolic execution engine.
class TaintModel {
public:
  TaintModel();

  void getTransferDstVect(const llvm::CallBase *CS, llvm::Value *Arg,
                          std::vector<llvm::Value *> &DstVect) const;

  bool isFunctionRetAsSource(const llvm::Function *Func) const;
  bool isFunctionArgAsSource(const llvm::Function *Func) const;
  const std::vector<int> *
  getTaintSourceArguments(llvm::Function *Func) const;

  bool isSinkFunction(const llvm::Function *Func) const;
  const std::vector<int> *
  getSinkArguments(const llvm::Function *Func) const;

  /// Evaluates taint source rules at instruction I using the enclosing GVFG.
  std::set<Var> getTaintedVars(llvm::Instruction *I,
                               lotus::gvfg::GuardedValueFlowGraph *Graph) const;

private:
  std::unordered_map<std::string, std::vector<int>> ArgAsSourceFunctions;
  std::set<std::string> RetAsSourceFunctions;
  std::multimap<std::string, std::vector<int>> DataTransferFunctions;
  std::unordered_map<std::string, std::vector<int>> SinkFunctions;
};

} // namespace SymbolicExecution
