#include "Verification/Sifa/SifaSymAbs.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"
#include "Verification/SymAbsAI/Core/Engine/Analyzer.h"
#include "Verification/SymAbsAI/Core/Foundation/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Foundation/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/Integration/FunctionContext.h"
#include "Verification/SymAbsAI/Core/Integration/ModuleContext.h"
#include "Verification/SymAbsAI/Utils/Config.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace lotus::sifa;

namespace {
bool profileEnabled() { return std::getenv("LOTUS_SIFA_PROFILE") != nullptr; }

/// Owns the SymAbsAI module/function/Z3 context alongside the returned
/// AbstractValue so Z3 expressions in `state` never outlive `z3::context`.
/// Declaration order ensures `state` is destroyed BEFORE `mctx`!
struct SymAbsResultOwner {
  std::unique_ptr<symabs_ai::ModuleContext> mctx;
  std::unique_ptr<symabs_ai::FunctionContext> fctx;
  std::unique_ptr<symabs_ai::FragmentDecomposition> fragDecomp;
  std::unique_ptr<symabs_ai::DomainConstructor> dom;
  std::unique_ptr<symabs_ai::Analyzer> analyzer;
  SymAbsState state;
};

bool domainNeedsBlockModel(llvm::StringRef spec) {
  llvm::SmallVector<llvm::StringRef, 8> parts;
  spec.split(parts, ',');
  for (llvm::StringRef part : parts) {
    llvm::StringRef trimmed = part.trim();
    if (trimmed == "ValidRegion" || trimmed == "ConstRegion" ||
        trimmed == "VarRegion" || trimmed == "MemRegion" ||
        trimmed == "NoAlias") {
      return true;
    }
  }
  return false;
}

void logProfileTiming(const char *stage,
                      std::chrono::steady_clock::duration elapsed) {
  if (!profileEnabled()) {
    return;
  }
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  llvm::errs() << "[sifa-profile] " << stage << ": " << ms << " ms\n";
}
} // namespace

static bool isSupportedSymAbsValueType(llvm::Type *ty,
                                       const SifaSymAbsOptions &opt) {
  if (!ty)
    return false;
  if (ty->isVoidTy())
    return true;
  // Non-value/control-only types that can appear as operands (e.g., branch
  // targets).
  if (ty->isLabelTy() || ty->isMetadataTy() || ty->isTokenTy())
    return true;
  if (ty->isPointerTy())
    return true;
  if (ty->isIntegerTy())
    return ty->getIntegerBitWidth() <= 64;
  if (opt.allowDouble && ty->isDoubleTy())
    return true;
  return false;
}

static bool isUnsupportedSymAbsInstruction(const llvm::Instruction &I) {
  // These are either not encoded at all (placeholder `true`) or rely on LLVM
  // exception handling machinery we currently don't model.
  return llvm::isa<llvm::IndirectBrInst>(I) || llvm::isa<llvm::InvokeInst>(I) ||
         llvm::isa<llvm::ResumeInst>(I) || llvm::isa<llvm::LandingPadInst>(I) ||
         llvm::isa<llvm::CleanupReturnInst>(I) ||
         llvm::isa<llvm::CatchReturnInst>(I) ||
         llvm::isa<llvm::CatchSwitchInst>(I) ||
         llvm::isa<llvm::FuncletPadInst>(I) ||
         llvm::isa<llvm::CleanupPadInst>(I) ||
         llvm::isa<llvm::CatchPadInst>(I) || llvm::isa<llvm::FenceInst>(I) ||
         llvm::isa<llvm::AtomicCmpXchgInst>(I) ||
         llvm::isa<llvm::AtomicRMWInst>(I) || llvm::isa<llvm::VAArgInst>(I) ||
         llvm::isa<llvm::ExtractElementInst>(I) ||
         llvm::isa<llvm::InsertElementInst>(I) ||
         llvm::isa<llvm::ShuffleVectorInst>(I) ||
         llvm::isa<llvm::ExtractValueInst>(I) ||
         llvm::isa<llvm::InsertValueInst>(I) ||
         llvm::isa<llvm::AddrSpaceCastInst>(I);
}

static void validateLlvmSubsetOrThrow(const llvm::Module &M,
                                      const llvm::Function &F,
                                      const SifaSymAbsOptions &opt) {
  if (!opt.validateLlvmSubset)
    return;

  std::vector<std::string> issues;
  issues.reserve(16);

  auto addIssue = [&](llvm::Twine msg) {
    if (issues.size() < 20)
      issues.push_back(msg.str());
  };

  // Validate `F` and, when recursive summaries are enabled, all reachable
  // defined direct callees.
  std::vector<const llvm::Function *> toValidate;
  std::unordered_set<const llvm::Function *> seenFuncs;
  std::queue<const llvm::Function *> fwork;
  toValidate.push_back(&F);
  seenFuncs.insert(&F);
  fwork.push(&F);
  while (opt.recursive && !fwork.empty()) {
    const llvm::Function *curF = fwork.front();
    fwork.pop();
    for (const llvm::BasicBlock &BB : *curF) {
      for (const llvm::Instruction &I : BB) {
        const auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!CB)
          continue;
        const llvm::Function *callee = CB->getCalledFunction();
        if (callee && !callee->isDeclaration() && !callee->empty() &&
            seenFuncs.insert(callee).second) {
          toValidate.push_back(callee);
          fwork.push(callee);
        }
      }
    }
  }

  const llvm::DataLayout &DL = M.getDataLayout();
  const unsigned ptrBits = std::max(1u, DL.getPointerSizeInBits(0));

  for (const llvm::Function *curF : toValidate) {
    std::string fnTag =
        (curF == &F) ? std::string()
                     : (" (in callee `" + curF->getName().str() + "`)");

    for (const llvm::Argument &A : curF->args()) {
      if (!isSupportedSymAbsValueType(A.getType(), opt)) {
        std::string s;
        llvm::raw_string_ostream os(s);
        os << "unsupported argument type" << fnTag << ": ";
        A.getType()->print(os);
        addIssue(os.str());
      }
    }
    if (!isSupportedSymAbsValueType(curF->getReturnType(), opt)) {
      std::string s;
      llvm::raw_string_ostream os(s);
      os << "unsupported return type" << fnTag << ": ";
      curF->getReturnType()->print(os);
      addIssue(os.str());
    }

    for (const llvm::BasicBlock &BB : *curF) {
      for (const llvm::Instruction &I : BB) {
        if (isUnsupportedSymAbsInstruction(I)) {
          addIssue(llvm::Twine("unsupported instruction") + fnTag + ": " +
                   llvm::Instruction::getOpcodeName(I.getOpcode()));
          continue;
        }

        // Types must be representable by SymAbsAI's Z3 encoding.
        if (!isSupportedSymAbsValueType(I.getType(), opt)) {
          std::string s;
          llvm::raw_string_ostream os(s);
          os << "unsupported instruction result type" << fnTag << " in "
             << I.getOpcodeName() << ": ";
          I.getType()->print(os);
          addIssue(os.str());
        }

        for (const llvm::Use &U : I.operands()) {
          llvm::Value *V = U.get();
          if (!V)
            continue;
          if (!isSupportedSymAbsValueType(V->getType(), opt)) {
            std::string s;
            llvm::raw_string_ostream os(s);
            os << "unsupported operand type" << fnTag << " used by "
               << I.getOpcodeName() << ": ";
            V->getType()->print(os);
            addIssue(os.str());
          }
        }

        // Floating-point casts are not supported (double-only model).
        if (llvm::isa<llvm::FPTruncInst>(I) || llvm::isa<llvm::FPExtInst>(I)) {
          addIssue(llvm::Twine("unsupported floating-point cast") + fnTag +
                   ": " + llvm::Instruction::getOpcodeName(I.getOpcode()));
        }

        // Avoid known crash in getelementptr encoding when index bitwidth > ptr
        // bitwidth.
        if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
          for (const auto *idxIt = GEP->idx_begin(); idxIt != GEP->idx_end();
               ++idxIt) {
            llvm::Value *Idx = idxIt->get();
            if (!Idx)
              continue;
            llvm::Type *Ty = Idx->getType();
            if (Ty->isIntegerTy() && Ty->getIntegerBitWidth() > ptrBits) {
              addIssue(llvm::Twine("unsupported gep index width (> pointer width)") +
                       fnTag);
              break;
            }
          }
        }

        if (issues.size() >= 20)
          break;
      }
      if (issues.size() >= 20)
        break;
    }
    if (issues.size() >= 20)
      break;
  }

  if (issues.empty())
    return;

  std::string msg;
  llvm::raw_string_ostream os(msg);
  os << "SifaSymAbs: unsupported LLVM IR for function `" << F.getName()
     << "`.\n";
  os << "Supported subset: integers (<=64-bit), pointers";
  if (opt.allowDouble)
    os << ", and double";
  os << ".\n";
  os << "Issues (first " << issues.size() << "):\n";
  for (const auto &s : issues)
    os << "  - " << s << "\n";
  throw std::invalid_argument(os.str());
}

static symabs_ai::configparser::Config
makeConfig(const SifaSymAbsOptions &opt) {
  symabs_ai::configparser::Config cfg;
  cfg.set("ModuleContext", "Recursive", opt.recursive);
  cfg.set("Analyzer", "Variant", opt.analyzerVariant);
  cfg.set("AbstractDomain", "Variant", opt.abstractDomain);
  cfg.set("FunctionContext", "RepresentAllInstructions",
          opt.representAllInstructions);
  if (domainNeedsBlockModel(opt.abstractDomain))
    cfg.set("MemoryModel", "Variant", llvm::StringRef("BlockModel"));
  if (opt.wideningDelay >= 0)
    cfg.set("Analyzer", "WideningDelay", opt.wideningDelay);
  if (opt.wideningFrequency >= 1)
    cfg.set("Analyzer", "WideningFrequency", opt.wideningFrequency);
  return cfg;
}

static SymAbsState runForTarget(const llvm::Module &M, const llvm::Function &F,
                                llvm::BasicBlock *target,
                                const SifaSymAbsOptions &options) {
  const auto overallStart = std::chrono::steady_clock::now();

  auto stageStart = std::chrono::steady_clock::now();
  validateLlvmSubsetOrThrow(M, F, options);
  logProfileTiming("validateLlvmSubset",
                   std::chrono::steady_clock::now() - stageStart);

  // Configure SifaLogger from options.
  SifaLogLevel level = options.logLevel;
  if (options.progressStream && level == SifaLogLevel::None)
    level = SifaLogLevel::Progress;
  if (level != SifaLogLevel::None) {
    SifaLogger::setOutputStream(options.progressStream ? options.progressStream
                                                       : &llvm::errs());
    SifaLogger::setLevel(level);
  }

  symabs_ai::Analyzer::resetBestTransformerCallCount();
  symabs_ai::Analyzer::resetSmtSolverCallCount();
  auto cfg = makeConfig(options);

  // SymAbsAI expects non-const pointers (it mutates analysis state
  // and queries IR properties through non-const APIs).
  auto *mod = const_cast<llvm::Module *>(&M);
  auto *fun = const_cast<llvm::Function *>(&F);

  auto owner = std::make_shared<SymAbsResultOwner>();

  SifaLogger::progress("Building module context...");
  stageStart = std::chrono::steady_clock::now();
  owner->mctx = std::make_unique<symabs_ai::ModuleContext>(mod, cfg);
  logProfileTiming("ModuleContext",
                   std::chrono::steady_clock::now() - stageStart);
  SifaLogger::progress("Building function context and analyzer...");
  stageStart = std::chrono::steady_clock::now();
  owner->fctx = owner->mctx->createFunctionContext(fun);
  owner->fragDecomp = std::make_unique<symabs_ai::FragmentDecomposition>(
      symabs_ai::FragmentDecomposition::For(*owner->fctx));
  const auto fcfg = owner->fctx->getConfig();
  owner->dom = std::make_unique<symabs_ai::DomainConstructor>(fcfg);
  owner->analyzer =
      symabs_ai::Analyzer::New(*owner->fctx, *owner->fragDecomp, *owner->dom);
  logProfileTiming("FunctionContext+Analyzer",
                   std::chrono::steady_clock::now() - stageStart);

  SifaStats stats;
  SifaSymAbsDomain domain(*owner->fctx, *owner->dom, *owner->analyzer);
  NeverFluid<SymAbsState> fluid;

  DagInterpreter<Transition, SymAbsState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, SymAbsState> loopSum(stats, domain, fluid,
                                                          ipr);
  ipr.setLoopSummarizer(loopSum);

  SifaLogger::progress("Building procedure resources (regex DAG)...");
  stageStart = std::chrono::steady_clock::now();
  ProcedureResources res(stats, *fun, {target});
  logProfileTiming("ProcedureResources",
                   std::chrono::steady_clock::now() - stageStart);
  auto initial = domain.makeTopAt(&fun->getEntryBlock(), /*after=*/false);

  SifaLogger::progress("Running fixpoint interpretation...");
  stageStart = std::chrono::steady_clock::now();
  // Interpret for the unique marker in the LOI overlay.
  owner->state = ipr.interpretForSingleMarker(
      res.getRegexDag(), res.getDagOverlayPathToLois(), initial);
  logProfileTiming("Interpret", std::chrono::steady_clock::now() - stageStart);
  SifaLogger::progress(
      "bestTransformer calls: " +
      std::to_string(symabs_ai::Analyzer::getBestTransformerCallCount()) +
      ", SMT solver calls: " +
      std::to_string(symabs_ai::Analyzer::getSmtSolverCallCount()));
  if (profileEnabled()) {
    llvm::errs() << "[sifa-profile] bestTransformer calls: "
                 << symabs_ai::Analyzer::getBestTransformerCallCount()
                 << ", SMT solver calls: "
                 << symabs_ai::Analyzer::getSmtSolverCallCount() << "\n";
    logProfileTiming("Total", std::chrono::steady_clock::now() - overallStart);
  }
  if (!owner->state)
    return nullptr;
  return SymAbsState(owner, owner->state.get());
}

static SymAbsState runForReturn(const llvm::Module &M, const llvm::Function &F,
                                const SifaSymAbsOptions &options) {
  const auto overallStart = std::chrono::steady_clock::now();

  auto stageStart = std::chrono::steady_clock::now();
  validateLlvmSubsetOrThrow(M, F, options);
  logProfileTiming("validateLlvmSubset",
                   std::chrono::steady_clock::now() - stageStart);

  // Configure SifaLogger from options.
  SifaLogLevel level = options.logLevel;
  if (options.progressStream && level == SifaLogLevel::None)
    level = SifaLogLevel::Progress;
  if (level != SifaLogLevel::None) {
    SifaLogger::setOutputStream(options.progressStream ? options.progressStream
                                                       : &llvm::errs());
    SifaLogger::setLevel(level);
  }

  symabs_ai::Analyzer::resetBestTransformerCallCount();
  symabs_ai::Analyzer::resetSmtSolverCallCount();
  auto cfg = makeConfig(options);

  auto *mod = const_cast<llvm::Module *>(&M);
  auto *fun = const_cast<llvm::Function *>(&F);

  auto owner = std::make_shared<SymAbsResultOwner>();

  SifaLogger::progress("Building module context...");
  stageStart = std::chrono::steady_clock::now();
  owner->mctx = std::make_unique<symabs_ai::ModuleContext>(mod, cfg);
  logProfileTiming("ModuleContext",
                   std::chrono::steady_clock::now() - stageStart);
  SifaLogger::progress("Building function context and analyzer...");
  stageStart = std::chrono::steady_clock::now();
  owner->fctx = owner->mctx->createFunctionContext(fun);
  owner->fragDecomp = std::make_unique<symabs_ai::FragmentDecomposition>(
      symabs_ai::FragmentDecomposition::For(*owner->fctx));
  const auto fcfg = owner->fctx->getConfig();
  owner->dom = std::make_unique<symabs_ai::DomainConstructor>(fcfg);
  owner->analyzer =
      symabs_ai::Analyzer::New(*owner->fctx, *owner->fragDecomp, *owner->dom);
  logProfileTiming("FunctionContext+Analyzer",
                   std::chrono::steady_clock::now() - stageStart);

  SifaStats stats;
  SifaSymAbsDomain domain(*owner->fctx, *owner->dom, *owner->analyzer);
  NeverFluid<SymAbsState> fluid;

  DagInterpreter<Transition, SymAbsState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, SymAbsState> loopSum(stats, domain, fluid,
                                                          ipr);
  ipr.setLoopSummarizer(loopSum);

  SifaLogger::progress("Building procedure resources (regex DAG)...");
  stageStart = std::chrono::steady_clock::now();
  ProcedureResources res(stats, *fun, std::vector<llvm::BasicBlock *>{});
  logProfileTiming("ProcedureResources",
                   std::chrono::steady_clock::now() - stageStart);
  auto initial = domain.makeTopAt(&fun->getEntryBlock(), /*after=*/false);

  SifaLogger::progress("Running fixpoint interpretation...");
  stageStart = std::chrono::steady_clock::now();
  owner->state = ipr.interpretForSingleMarker(
      res.getRegexDag(), res.getDagOverlayPathToReturn(), initial);
  logProfileTiming("Interpret", std::chrono::steady_clock::now() - stageStart);
  SifaLogger::progress(
      "bestTransformer calls: " +
      std::to_string(symabs_ai::Analyzer::getBestTransformerCallCount()) +
      ", SMT solver calls: " +
      std::to_string(symabs_ai::Analyzer::getSmtSolverCallCount()));
  if (profileEnabled()) {
    llvm::errs() << "[sifa-profile] bestTransformer calls: "
                 << symabs_ai::Analyzer::getBestTransformerCallCount()
                 << ", SMT solver calls: "
                 << symabs_ai::Analyzer::getSmtSolverCallCount() << "\n";
    logProfileTiming("Total", std::chrono::steady_clock::now() - overallStart);
  }
  if (!owner->state)
    return nullptr;
  return SymAbsState(owner, owner->state.get());
}

SymAbsState lotus::sifa::analyzeSymAbsTo(const llvm::Module &M,
                                         const llvm::Function &F,
                                         const llvm::BasicBlock &target,
                                         const SifaSymAbsOptions &options) {
  return runForTarget(M, F, const_cast<llvm::BasicBlock *>(&target), options);
}

bool lotus::sifa::isReachableSymAbs(const llvm::Module &M,
                                    const llvm::Function &F,
                                    const llvm::BasicBlock &target,
                                    const SifaSymAbsOptions &options) {
  const SymAbsState out = analyzeSymAbsTo(M, F, target, options);
  return out && !out->isBottom();
}

SymAbsState
lotus::sifa::analyzeSymAbsToReturn(const llvm::Module &M,
                                   const llvm::Function &F,
                                   const SifaSymAbsOptions &options) {
  return runForReturn(M, F, options);
}
