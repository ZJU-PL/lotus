#pragma once

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "Analysis/DebugInfo/IRExpressionRenderer.h"

#include <map>
#include <utility>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

namespace lotus {
using namespace llvm;

using namespace llvm;

class CastFailureAnalysis {
public:
  CastFailureAnalysis();
  virtual ~CastFailureAnalysis();

private:
  // Check if the \p kth operand of \p phi_node is from a cast failure
  bool isCastFailNullForPhi(const PHINode *phi_node, int kth);

public:
  // Check if the PHINode \p phi_node is a cast failure nullptr as its argument
  bool phiHasCastFailNullArg(const PHINode *phi_node);

  // Check if the compare instruction is for defending NULL cast
  bool isCastNullCheckBranch(const Instruction *);

public:
  bool analyze(llvm::Module &, DebugInfoAnalysis &,
               ir_expression::IRExpressionRenderer &);

private:
  DebugInfoAnalysis *DIA = nullptr;
  ir_expression::IRExpressionRenderer *inst_resolver = nullptr;

  // Map PHI to the NULL operands index that come from C style cast failures
  // Use a multimap is more efficient because the amount of cast failure PHI
  // nodes is not too many
  std::multimap<const PHINode *, int> cast_fail_phi_oprds;
};

} // namespace lotus
