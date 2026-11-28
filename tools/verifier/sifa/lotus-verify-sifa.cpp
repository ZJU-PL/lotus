// lotus-verify-sifa.cpp - Sifa (Symbolic Interpretation with Fluid Abstractions) verifier tool.
// ./build/bin/lotus-verify-sifa tmp/sifa-demo/loop.bc --log-level=progress --symabs
//  Build bitcode without optnone so mem2reg can promote allocas to SSA

#include "Verification/Sifa/Sifa.h"

#include "Verification/Sifa/Domain/EqDomain.h"
#include "Verification/Sifa/Domain/ExplicitValueDomain.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/SifaSymAbs.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/SymAbsAI/Core/Foundation/AbstractValue.h"
#include "Verification/SymAbsAI/Core/Foundation/DomainConstructor.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"

#include <memory>
#include <string>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::sifa;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode>"),
                                          cl::value_desc("bitcode"));

static cl::opt<std::string>
    FunctionName("function", cl::desc("Function to analyze (default: main)"),
                 cl::value_desc("name"));

static cl::opt<std::string>
    BlockLabel("block",
               cl::desc("Block name or index (default: return)"),
               cl::value_desc("label"));

static cl::opt<std::string>
    DomainOpt("abstract-domain",
              cl::desc("Native: Interval, Octagon, Eq, ExplicitValue, both; "
                       "or SymAbsAI domain(s) with --symabs"),
              cl::value_desc("domain"), cl::init("Interval"));

static cl::opt<bool> UseSymAbs("symabs",
                               cl::desc("Use SMT-backed SymbolicAbstraction"),
                               cl::init(false));

static cl::opt<bool>
    RepresentAll("represent-all",
                 cl::desc("Represent all SSA values in the invariant (e.g. i, "
                          "s); otherwise only named values"),
                 cl::init(false));

static cl::opt<int> WideningDelay(
    "widening-delay",
    cl::desc("Delay before first widening in SMT loop (higher => more precise "
             "intervals); default 20 for --symabs"),
    cl::init(-1));

static cl::opt<int> WideningFrequency(
    "widening-frequency",
    cl::desc("Widening frequency after delay (only with --symabs)"),
    cl::init(-1));

static cl::opt<bool> ReachabilityOnly("reachability",
                                      cl::desc("Only check reachability"),
                                      cl::init(false));

static cl::opt<bool> NoValidateSubset("no-validate-subset",
                                      cl::desc("Disable IR subset validation"),
                                      cl::init(false));

static cl::opt<bool> ListFunctions("list-functions", cl::desc("List functions"),
                                   cl::init(false));

static cl::opt<bool> ListBlocks("list-blocks", cl::desc("List basic blocks"),
                                cl::init(false));

static cl::opt<bool>
    ListDomains("list-domains",
                cl::desc("List available native and SymAbsAI abstract domains"),
                cl::init(false));

static cl::opt<bool> Verbose("verbose",
                             cl::desc("Print detailed state (SymAbs)"),
                             cl::init(false));

static cl::opt<std::string>
    LogLevel("log-level",
             cl::desc("none|error|warning|info|progress|debug|trace"),
             cl::value_desc("level"), cl::init("none"));

static llvm::BasicBlock *findBlock(llvm::Function &F,
                                   const std::string &label) {
  for (auto &BB : F)
    if (BB.hasName() && BB.getName() == label)
      return &BB;

  StringRef sr(label);
  if (sr.startswith("[") && sr.endswith("]") && sr.size() > 2)
    sr = sr.drop_front(1).drop_back(1);
  else if (sr.startswith("#") && sr.size() > 1)
    sr = sr.drop_front(1);

  unsigned idx = 0;
  if (!sr.empty() && !sr.getAsInteger(10, idx)) {
    unsigned cur = 0;
    for (auto &BB : F) {
      if (cur == idx)
        return &BB;
      ++cur;
    }
  }
  return nullptr;
}

static SifaLogLevel parseLogLevel(const std::string &s) {
  const std::pair<std::string, SifaLogLevel> levels[] = {
      {"error", SifaLogLevel::Error}, {"warning", SifaLogLevel::Warning},
      {"info", SifaLogLevel::Info},   {"progress", SifaLogLevel::Progress},
      {"debug", SifaLogLevel::Debug}, {"trace", SifaLogLevel::Trace}};
  for (const auto &p : levels)
    if (s == p.first)
      return p.second;
  return SifaLogLevel::None;
}

template <typename DomainT, typename StateT>
static StateT analyzeToReturnGeneric(const llvm::Function &F,
                                     const StateT &initial,
                                     const DomainT &domain) {
  SifaStats stats;
  NeverFluid<StateT> fluid;
  DagInterpreter<Transition, StateT> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, StateT> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);
  ProcedureResources res(stats, F, std::vector<llvm::BasicBlock *>{});
  return ipr.interpretForSingleMarker(res.getRegexDag(),
                                      res.getDagOverlayPathToReturn(), initial);
}

static bool validateSymAbsDomainSpec(const std::string &spec,
                                     std::string &errorMsg) {
  const auto &allDomains = symabs_ai::DomainConstructor::all();
  llvm::SmallVector<StringRef, 4> pieces;
  StringRef(spec).split(pieces, ',');
  unsigned validCount = 0;
  for (StringRef piece : pieces) {
    StringRef trimmed = piece.trim();
    if (trimmed.empty())
      continue;
    bool found = false;
    for (const auto &d : allDomains) {
      if (trimmed == d.name()) {
        found = true;
        break;
      }
    }
    if (!found) {
      errorMsg = "Unknown SymAbsAI abstract domain '" + trimmed.str() +
                 "'. Use --list-domains to see available options.";
      return false;
    }
    ++validCount;
  }
  if (validCount == 0) {
    errorMsg = "Empty --abstract-domain specification.";
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(
      argc, argv, "Sifa - Symbolic Interpretation with Fluid Abstractions\n");

  if (ListDomains) {
    outs() << "Native Sifa domains:\n"
           << "  Interval\n"
           << "  Octagon\n"
           << "  Eq\n"
           << "  ExplicitValue\n"
           << "  both (Interval + Octagon)\n\n"
           << "SymAbsAI domains (--symabs):\n";
    for (const auto &d : symabs_ai::DomainConstructor::all()) {
      outs() << "  " << d.name();
      if (!d.description().empty())
        outs() << " - " << d.description();
      outs() << "\n";
    }
    return 0;
  }

  SifaLogLevel logLevel = parseLogLevel(LogLevel.getValue());
  if (logLevel != SifaLogLevel::None) {
    SifaLogger::setOutputStream(&errs());
    SifaLogger::setLevel(logLevel);
  }

  if (InputFilename.empty()) {
    errs() << "Error: input bitcode required.\n";
    return 1;
  }
  SifaLogger::progress("Loading bitcode: " + InputFilename.getValue());

  LLVMContext context;
  SMDiagnostic err;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);
  if (!module) {
    err.print(argv[0], errs());
    return 1;
  }
  SifaLogger::progress("Module loaded");

  if (ListFunctions) {
    for (auto &F : *module)
      if (!F.isDeclaration())
        outs() << "  " << F.getName() << "\n";
    return 0;
  }

  Function *targetFunc = nullptr;
  if (!FunctionName.empty()) {
    targetFunc = module->getFunction(FunctionName);
    if (!targetFunc) {
      errs() << "Error: Function '" << FunctionName.getValue()
             << "' not found\n";
      return 1;
    }
  } else {
    targetFunc = module->getFunction("main");
    if (!targetFunc || targetFunc->isDeclaration() || targetFunc->empty()) {
      targetFunc = nullptr;
      for (auto &F : *module) {
        if (!F.isDeclaration() && !F.empty()) {
          targetFunc = &F;
          break;
        }
      }
    }
    if (!targetFunc) {
      errs() << "Error: Function 'main' (or any defined function) not found\n";
      return 1;
    }
  }

  if (ListBlocks) {
    if (targetFunc->isDeclaration()) {
      outs() << "  (declaration - no basic blocks)\n";
      return 0;
    }
    unsigned idx = 0;
    for (auto &BB : *targetFunc) {
      outs() << "  [" << idx++ << "] "
             << (BB.hasName() ? BB.getName() : "(unnamed)") << "\n";
    }
    return 0;
  }

  if (targetFunc->isDeclaration() || targetFunc->empty()) {
    errs() << "Error: Function '" << targetFunc->getName()
           << "' has no body (declaration or empty)\n";
    return 1;
  }

  SifaOptions sifaOpts;
  try {
    if (UseSymAbs) {
      std::string domainErr;
      if (!validateSymAbsDomainSpec(DomainOpt.getValue(), domainErr)) {
        errs() << "Error: " << domainErr << "\n";
        return 1;
      }
      if (WideningFrequency.getNumOccurrences() > 0 &&
          WideningFrequency.getValue() <= 0) {
        errs() << "Error: --widening-frequency must be >= 1\n";
        return 1;
      }

      SifaSymAbsOptions options;
      options.abstractDomain = DomainOpt.getValue();
      options.representAllInstructions = RepresentAll;
      options.wideningDelay =
          WideningDelay.getNumOccurrences() ? WideningDelay.getValue() : 20;
      options.wideningFrequency = WideningFrequency.getValue();
      options.validateLlvmSubset = !NoValidateSubset;
      options.logLevel = logLevel;
      if (logLevel >= SifaLogLevel::Progress)
        options.progressStream = &errs();

      unsigned nBlocks = 0;
      for (auto &BB : *targetFunc)
        (void)BB, ++nBlocks;
      outs() << "Abstract domain(s): " << options.abstractDomain << "\n";
      SifaLogger::progress("Analyzing (SymAbs/SMT) function '" +
                           targetFunc->getName().str() + "' (" +
                           std::to_string(nBlocks) +
                           " blocks, domain: " + options.abstractDomain + ")");

      auto reportSymAbs = [&](const std::string &label, SymAbsState state) {
        if (ReachabilityOnly) {
          bool reachable = state && !state->isBottom();
          outs() << label
                 << (reachable ? " is reachable\n" : " is not reachable\n");
          return;
        }
        if (!state || state->isBottom()) {
          outs() << label << ": "
                 << (!state ? "unreachable (bottom)" : "bottom") << "\n";
          return;
        }
        outs() << label << ": state (top/non-bottom)\n";
        outs() << "Final invariant:\n";
        symabs_ai::PrettyPrinter pp(/*output_html=*/Verbose);
        state->prettyPrint(pp);
        outs() << pp.str() << "\n";
      };

      if (!BlockLabel.empty()) {
        BasicBlock *targetBlock = findBlock(*targetFunc, BlockLabel.getValue());
        if (!targetBlock) {
          errs() << "Error: block '" << BlockLabel.getValue()
                 << "' not found\n";
          return 1;
        }
        SymAbsState state =
            analyzeSymAbsTo(*module, *targetFunc, *targetBlock, options);
        reportSymAbs("Block " + BlockLabel.getValue(), std::move(state));
      } else {
        reportSymAbs("Return",
                     analyzeSymAbsToReturn(*module, *targetFunc, options));
      }
    } else {
      StringRef domSpec = StringRef(DomainOpt.getValue()).trim();
      const bool isInterval =
          domSpec.empty() || domSpec.equals_insensitive("Interval");
      const bool isOctagon = domSpec.equals_insensitive("Octagon");
      const bool isEq = domSpec.equals_insensitive("Eq");
      const bool isExplicit = domSpec.equals_insensitive("ExplicitValue");
      const bool isBoth = domSpec.equals_insensitive("both");

      if (!isInterval && !isOctagon && !isEq && !isExplicit && !isBoth) {
        errs() << "Error: unknown native abstract domain '" << DomainOpt.getValue()
               << "'. Supported: Interval, Octagon, Eq, ExplicitValue, both\n";
        return 1;
      }

      BasicBlock *targetBlock = nullptr;
      if (!BlockLabel.empty()) {
        targetBlock = findBlock(*targetFunc, BlockLabel.getValue());
        if (!targetBlock) {
          errs() << "Error: block '" << BlockLabel.getValue()
                 << "' not found\n";
          return 1;
        }
      }

      unsigned nBlocks = 0;
      for (auto &BB : *targetFunc)
        (void)BB, ++nBlocks;
      SifaLogger::progress(
          "Analyzing (instruction transfer, no SMT) function '" +
          targetFunc->getName().str() + "' (" + std::to_string(nBlocks) +
          " blocks, domain: " + domSpec.str() + ")");

      auto label = BlockLabel.empty() ? std::string("Return")
                                      : "Block " + BlockLabel.getValue();
      auto reportNative = [&](bool bottom, auto &state, const char *dom) {
        if (ReachabilityOnly) {
          outs() << label
                 << (bottom ? " is not reachable\n" : " is reachable\n");
          return;
        }
        if (bottom) {
          outs() << label << ": unreachable (bottom)\n";
          return;
        }
        outs() << label << ": state (" << dom << " domain)\n";
        state.print(outs());
      };

      if (isInterval || isBoth) {
        IntervalState state =
            targetBlock ? analyzeToWithIntervalDomain(
                              *targetFunc, *targetBlock, IntervalState(false),
                              sifaOpts)
                        : analyzeToReturnWithIntervalDomain(
                              *targetFunc, IntervalState(false), sifaOpts);
        IntervalDomain dom(nullptr, nullptr);
        reportNative(dom.isBottom(state), state, "interval");
      }
      if (isOctagon || isBoth) {
        OctagonState state =
            targetBlock
                ? analyzeToWithOctagonDomain(*targetFunc, *targetBlock,
                                             OctagonState(false), sifaOpts)
                : analyzeToReturnWithOctagonDomain(
                      *targetFunc, OctagonState(false), sifaOpts);
        OctagonDomain dom(nullptr, nullptr);
        reportNative(dom.isBottom(state), state, "octagon");
      }
      if (isEq) {
        EqDomain dom(nullptr, nullptr);
        EqState state =
            targetBlock ? analyzeToWithEqDomain(*targetFunc, *targetBlock,
                                                EqState(false), sifaOpts)
                        : analyzeToReturnGeneric(*targetFunc, EqState(false),
                                                 dom);
        if (ReachabilityOnly) {
          outs() << label
                 << (dom.isBottom(state) ? " is not reachable\n"
                                         : " is reachable\n");
        } else if (dom.isBottom(state)) {
          outs() << label << ": unreachable (bottom)\n";
        } else {
          outs() << label << ": state (eq domain)\n"
                 << "  tracked values: " << state.keys().size();
          if (!state.memory().empty())
            outs() << ", memory regions: " << state.memory().size();
          outs() << "\n";
        }
      }
      if (isExplicit) {
        ExplicitValueDomain dom(nullptr, nullptr);
        ExplicitValueState state =
            targetBlock
                ? analyzeToWithExplicitValueDomain(
                      *targetFunc, *targetBlock, ExplicitValueState(false),
                      sifaOpts)
                : analyzeToReturnGeneric(*targetFunc, ExplicitValueState(false),
                                         dom);
        if (ReachabilityOnly) {
          outs() << label
                 << (dom.isBottom(state) ? " is not reachable\n"
                                         : " is reachable\n");
        } else if (dom.isBottom(state)) {
          outs() << label << ": unreachable (bottom)\n";
        } else {
          outs() << label << ": state (explicit value domain)\n"
                 << "  constants: " << state.map_.size();
          if (!state.memory_.empty())
            outs() << ", memory regions: " << state.memory_.size();
          outs() << "\n";
        }
      }
    }
    outs() << "Sifa analysis completed successfully.\n";
  } catch (const std::exception &e) {
    errs() << "Error during analysis: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
