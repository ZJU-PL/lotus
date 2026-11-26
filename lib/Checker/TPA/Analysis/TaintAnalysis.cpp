#include "Checker/TPA/Analysis/TaintAnalysis.h"

#include "Checker/TPA/Engine/Initializer.h"
#include "Checker/TPA/Engine/SinkViolationChecker.h"
#include "Checker/TPA/Engine/TaintGlobalState.h"
#include "Checker/TPA/Engine/TaintPropagator.h"
#include "Checker/TPA/Engine/TransferFunction.h"
#include "Checker/TPA/Output/Printer.h"
#include "Checker/TPA/Precision/PrecisionLossTracker.h"
#include "Checker/TPA/Program/DefUseModule.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

namespace tpa::taint {
bool verifyInputModule(llvm::Module &module, llvm::raw_ostream &errors,
                       bool &debugStripped) {
  debugStripped = false;
  bool brokenDebug = false;
  // First inspect without printing Values: debug diagnostics can otherwise
  // rebuild large module slot tables for every affected call site.
  if (llvm::verifyModule(module, nullptr, &brokenDebug)) {
    llvm::verifyModule(module, &errors, &brokenDebug);
    errors << "Input LLVM IR verification failed\n";
    return false;
  }
  if (brokenDebug) {
    llvm::StripDebugInfo(module);
    debugStripped = true;
    return !llvm::verifyModule(module, &errors);
  }
  return true;
}

bool validateModule(const llvm::Module &module, llvm::raw_ostream &errors) {
  auto *main = module.getFunction("main");
  if (!main || main->isDeclaration()) {
    errors << "TPA taint requires a defined main entry point\n";
    return false;
  }
  bool valid = true;
  size_t unsupported = 0;
  constexpr size_t maxDiagnostics = 20;
  for (const auto &function : module)
    for (const auto &block : function)
      for (const auto &inst : block) {
        // Explicit-flow, path-insensitive semantics. Exception type matching,
        // concurrent scheduling and variadic function bodies remain partial.
        bool supported = inst.isBinaryOp() || inst.isCast() ||
                         llvm::isa<llvm::CmpInst>(inst);
        switch (inst.getOpcode()) {
        case llvm::Instruction::FNeg:
        case llvm::Instruction::Freeze:
        case llvm::Instruction::IndirectBr:
        case llvm::Instruction::AtomicRMW:
        case llvm::Instruction::AtomicCmpXchg:
        case llvm::Instruction::Fence:
        case llvm::Instruction::Alloca:
        case llvm::Instruction::Load:
        case llvm::Instruction::Store:
        case llvm::Instruction::GetElementPtr:
        case llvm::Instruction::PHI:
        case llvm::Instruction::Select:
        case llvm::Instruction::ExtractElement:
        case llvm::Instruction::ExtractValue:
        case llvm::Instruction::InsertElement:
        case llvm::Instruction::InsertValue:
        case llvm::Instruction::ShuffleVector:
        case llvm::Instruction::Invoke:
        case llvm::Instruction::CallBr:
        case llvm::Instruction::LandingPad:
        case llvm::Instruction::Resume:
        case llvm::Instruction::Call:
        case llvm::Instruction::Ret:
        case llvm::Instruction::Br:
        case llvm::Instruction::Switch:
        case llvm::Instruction::Unreachable:
          supported = true;
          break;
        default:
          break;
        }
        // Pointer-valued RMW/cmpxchg also need pointer-store semantics in
        // the TPA backend. Until those exist, reject them explicitly.
        bool pointerAtomic = false;
        if (auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&inst))
          pointerAtomic = rmw->getValOperand()->getType()->isPointerTy();
        if (auto *cx = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&inst))
          pointerAtomic = cx->getCompareOperand()->getType()->isPointerTy();
        if (!supported || pointerAtomic) {
          if (++unsupported <= maxDiagnostics)
            // Printing the Value constructs a ModuleSlotTracker and can rescan
            // all debug metadata for every error. Opcode/name reads are linear.
            errors << "Unsupported TPA taint instruction in "
                   << function.getName() << ": " << inst.getOpcodeName()
                   << "\n";
          valid = false;
        }
      }
  if (unsupported > maxDiagnostics)
    errors << (unsupported - maxDiagnostics)
           << " additional unsupported instructions omitted (" << unsupported
           << " total)\n";
  return valid;
}

bool TaintAnalysis::runOnDefUseModule(const DefUseModule &module,
                                      bool trackPrecisionLoss) {
  if (!validateModule(module.getModule(), llvm::errs()))
    throw std::invalid_argument("Unsupported module for TPA taint");
  env = TaintEnv();
  memo = TaintMemo();
  violations.clear();
  lossSites = ProgramPointSet();
  stats = Statistics();
  ignoredUnresolvedCalls.clear();
  TaintGlobalState state(module, ptrAnalysis, extTable, env, memo,
                         unmodeledPolicy);
  auto work = Initializer(state, memo).runOnInitState(TaintStore());
  std::set<const context::Context *> contexts;
  while (!work.empty()) {
    auto point = work.dequeue();
    contexts.insert(point.getContext());
    auto result = TransferFunction(state, memo.lookup(point)).eval(point);
    ++stats.evaluations;
    TaintPropagator(memo, work).propagate(result);
  }
  ignoredUnresolvedCalls = state.getIgnoredUnresolvedCalls();
  violations = SinkViolationChecker(env, memo, extTable, ptrAnalysis)
                   .checkSinkViolation(state.getSinks());
  if (trackPrecisionLoss)
    lossSites = PrecisionLossTracker(state).trackImprecision(violations);
  stats.contexts = contexts.size();
  stats.valueFacts = env.size();
  stats.memoryStates = std::distance(memo.begin(), memo.end());
  stats.sinks = state.getSinks().size();
  for (const auto &entry : violations)
    stats.violations += entry.second.size();
  auto countEdges = [&](const DefUseInstruction &node) {
    for (auto *succ : node.top_succs()) {
      (void)succ;
      ++stats.topEdges;
    }
    for (const auto &entry : node.mem_succs())
      stats.memoryEdges += entry.second.size();
  };
  for (const auto &function : module) {
    countEdges(*function.getEntryInst());
    for (const auto &block : function.getFunction())
      for (const auto &inst : block)
        if (auto *node = function.getDefUseInstruction(&inst))
          countEdges(*node);
  }
  return violations.empty();
}

void TaintAnalysis::printResults(llvm::raw_ostream &out) const {
  // Sort rendered findings so unordered container iteration does not reorder
  // the same experiment's output. Context printers use IR, not addresses.
  using namespace util::io;
  std::vector<std::string> findings;
  for (const auto &entry : violations)
    for (const auto &violation : entry.second) {
      std::string text;
      llvm::raw_string_ostream os(text);
      os << "Sink violation at " << entry.first << "\n"
         << "  Argument: " << violation.argPos << "\n"
         << "  Expected: " << violation.expectVal << "\n"
         << "  Actual: " << violation.actualVal << "\n";
      findings.push_back(os.str());
    }
  std::sort(findings.begin(), findings.end());
  for (const auto &finding : findings)
    out << finding;
  std::vector<std::string> losses;
  for (const auto &point : lossSites) {
    std::string text;
    llvm::raw_string_ostream os(text);
    os << "Precision loss at " << point << "\n";
    losses.push_back(os.str());
  }
  std::sort(losses.begin(), losses.end());
  for (const auto &loss : losses)
    out << loss;
}
} // namespace tpa::taint
