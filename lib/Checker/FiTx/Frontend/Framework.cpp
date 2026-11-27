// FiTx framework pass: typestate-based daily development-friendly bug checkers.
// Based on: Suzuki et al., "Balancing Analysis Time and Bug Detection: Daily
// Development-friendly Bug Detection in Linux", USENIX ATC 2024 (Section 4, 5).
#include "Checker/FiTx/Frontend/Framework.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Core/SFG/Converter.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/FrameworkIR/IRGenerator.h"
#include "Checker/FiTx/Frontend/Analyzer.h"
#include "Checker/FiTx/Frontend/State.h"

namespace fitx {

FrameworkPass::FrameworkPass() : ModulePass(ID) {}

void FrameworkPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<ir_generator::IRGenerator>();
}

// Entry: each subclass overrides defineStates() to register StateManagers;
// run one Analyzer at a time in a local scope so per-checker state is released
// immediately between checkers instead of accumulating across all detectors.
bool FrameworkPass::runOnModule(llvm::Module &M) {
  manager_.clear();
  defineStates();

  for (fitx::StateManager &manager : manager_) {
    fitx::LoggingClient client;
    fitx::Analyzer analyzer(M, manager, client);
    analyzer.analyze();
    client.printLog();
  }

  // Release cached module IR and value registries so repeated runs in the same
  // process (e.g. unit tests) never retain dangling LLVM pointers or leak RAM.
  ir_generator::IRGenerator::clearModule(&M);
  fitx::Function::clearManagedFunctions();
  fitx::Converter::GetInstance().clear();
  fitx::ManagedValues::GetInstance().clear();

  return false;
}
} // namespace fitx

char fitx::FrameworkPass::ID = 0;
