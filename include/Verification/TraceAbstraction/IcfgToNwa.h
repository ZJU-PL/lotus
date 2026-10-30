#pragma once

#include "Verification/Automata/NestedWordAutomaton.h"

#include <unordered_map>

class ICFG;
class ICFGNode;
namespace llvm {
class Function;
class Instruction;
class BasicBlock;
} // namespace llvm

namespace lotus::verification::traceabstraction {
namespace nwa = automata;

// A letter identifies an instruction AND its control-flow outcome. In
// particular, the two edges of a conditional branch must never share a letter.
struct Statement {
  const llvm::Instruction *instruction = nullptr;
  const llvm::BasicBlock *successor = nullptr;
  const llvm::Function *callee = nullptr;
  nwa::LetterKind kind = nwa::LetterKind::Internal;
  bool error = false; // reaching this instruction, before executing its effect
  bool initialize = false; // module initial state, executed once at the root
};

struct ProgramAutomaton {
  std::shared_ptr<nwa::NestedWordAutomaton> automaton;
  std::vector<Statement> statements; // indexed by Letter
  std::unordered_map<const ICFGNode *, nwa::State> nodeStates;
  std::vector<const llvm::Function *> functions;
  std::shared_ptr<ICFG>
      ownedIcfg; // keeps Function-overload node mappings alive
};

class IcfgToNwa {
public:
  static ProgramAutomaton build(const llvm::Function &function,
                                const llvm::Instruction *targetError = nullptr,
                                nwa::Context context = nwa::defaultContext());
  // Starts at the graph's global root, or its function entries if no global
  // root exists. errorNode denotes entry to that node; nullptr detects the
  // standard error calls (__assert_fail, abort, __VERIFIER_error).
  static ProgramAutomaton build(const ICFG &icfg,
                                const ICFGNode *errorNode = nullptr,
                                nwa::Context context = nwa::defaultContext());
};
} // namespace lotus::verification::traceabstraction
