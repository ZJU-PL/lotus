#include "Checker/TPA/Engine/TransferFunction.h"

#include "Alias/InclusionBased/TPA/Context/ContextPolicy.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/FunctionContext.h"
#include "Checker/TPA/Engine/TaintGlobalState.h"
#include "Checker/TPA/Output/Printer.h"
#include "Checker/TPA/Program/DefUseModule.h"
#include "Checker/TPA/Program/Reachability.h"
#include "Checker/TPA/Support/ProgramPoint.h"
#include "Checker/TPA/Support/TaintEnv.h"

#include <cstddef>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace tpa;
using namespace util::io;

namespace tpa::taint {

void TransferFunction::addTopLevelSuccessors(const ProgramPoint &pp,
                                             EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  const auto *duInst = pp.getDefUseInstruction();
  for (auto *succ : duInst->top_succs())
    evalResult.addTopLevelSuccessor(ProgramPoint(ctx, succ));
}

void TransferFunction::addMemBroadcast(const ProgramPoint &srcPP,
                                       const ProgramPoint &dstPP,
                                       EvalResult &evalResult) {
  const bool full = syncState ? syncState->markBroadcast(srcPP, dstPP) : true;
  evalResult.addMemBroadcast(dstPP, full);
}

TaintLattice TransferFunction::getTaintForOperands(const context::Context *ctx,
                                                   const Instruction *inst) {
  TaintLattice currVal = TaintLattice::Unknown;
  auto &env = globalState.getEnv();
  for (auto i = 0u, e = inst->getNumOperands(); i < e; ++i) {
    auto *op = inst->getOperand(i);
    auto opVal = env.lookup(TaintValue(ctx, op));
    currVal = Lattice<TaintLattice>::merge(currVal, opVal);
  }
  return currVal;
}

void TransferFunction::evalEntry(const ProgramPoint &srcPP,
                                 const ProgramPoint &entryPP,
                                 EvalResult &evalResult, bool envChanged) {
  if (envChanged)
    addTopLevelSuccessors(entryPP, evalResult);
  addMemBroadcast(srcPP, entryPP, evalResult);
}

void TransferFunction::evalAlloca(const ProgramPoint &pp,
                                  EvalResult &evalResult) {
  const auto *duInst = pp.getDefUseInstruction();

  auto envChanged = globalState.getEnv().strongUpdate(
      TaintValue(pp.getContext(), duInst->getInstruction()),
      TaintLattice::Untainted);

  if (envChanged)
    addTopLevelSuccessors(pp, evalResult);
}

void TransferFunction::evalAllOperands(const ProgramPoint &pp,
                                       EvalResult &evalResult) {
  auto tVal = getTaintForOperands(pp.getContext(),
                                  pp.getDefUseInstruction()->getInstruction());

  if (tVal != TaintLattice::Unknown) {
    auto envChanged = globalState.getEnv().strongUpdate(
        TaintValue(pp.getContext(),
                   pp.getDefUseInstruction()->getInstruction()),
        tVal);
    if (envChanged)
      addTopLevelSuccessors(pp, evalResult);
  }
}

TaintLattice TransferFunction::loadTaintFromPtsSet(tpa::PtsSet pSet,
                                                   const TaintStore &store) {
  TaintLattice resVal = TaintLattice::Unknown;
  for (const auto *obj : pSet) {
    if (obj->isUniversalObject()) {
      resVal = TaintLattice::Either;
      break;
    } else if (obj->isNullObject())
      continue;

    auto locVal = store.lookup(obj);
    resVal = Lattice<TaintLattice>::merge(resVal, locVal);
  }

  return resVal;
}

void TransferFunction::evalLoad(const ProgramPoint &pp,
                                EvalResult &evalResult) {
  if (localState == nullptr)
    return;

  const auto *ctx = pp.getContext();
  const auto *loadInst =
      cast<LoadInst>(pp.getDefUseInstruction()->getInstruction());

  const auto *ptrOp = loadInst->getPointerOperand();
  auto ptsSet = globalState.getPointerAnalysis().getPtsSet(ctx, ptrOp);
  if (ptsSet.empty())
    return;

  auto loadVal = loadTaintFromPtsSet(ptsSet, *localState);
  if (loadVal != TaintLattice::Unknown) {
    auto envChanged = globalState.getEnv().strongUpdate(
        TaintValue(pp.getContext(),
                   pp.getDefUseInstruction()->getInstruction()),
        loadVal);
    if (envChanged)
      addTopLevelSuccessors(pp, evalResult);
  }
}

void TransferFunction::weakUpdateStore(PtsSet pSet, TaintLattice v,
                                       EvalResult &evalResult) {
  for (const auto *obj : pSet) {
    if (obj->isSpecialObject())
      continue;
    evalResult.weakUpdateOverride(obj, v);
  }
}

void TransferFunction::evalStore(const ProgramPoint &pp,
                                 EvalResult &evalResult) {
  evalResult.setBaseStore(localState);

  const auto *ctx = pp.getContext();
  const auto *storeInst =
      cast<StoreInst>(pp.getDefUseInstruction()->getInstruction());

  const auto *valOp = storeInst->getValueOperand();
  const auto *ptrOp = storeInst->getPointerOperand();

  auto val = globalState.getEnv().lookup(TaintValue(ctx, valOp));
  if (val == TaintLattice::Unknown)
    return;

  auto ptsSet = globalState.getPointerAnalysis().getPtsSet(ctx, ptrOp);
  if (ptsSet.empty())
    return;
  const auto *obj = *ptsSet.begin();

  if (ptsSet.size() == 1 && !obj->isSpecialObject() && !obj->isSummaryObject())
    evalResult.strongUpdateOverride(obj, val);
  else
    weakUpdateStore(ptsSet, val, evalResult);

  for (const auto *dstObj : ptsSet)
    evalResult.addDirectMemProp(pp, dstObj);
}

std::vector<TaintLattice> TransferFunction::collectArgumentTaintValue(
    const context::Context *ctx, const CallBase *cs, size_t numParam) {
  std::vector<TaintLattice> callerVals;
  callerVals.reserve(numParam);

  auto const &env = globalState.getEnv();
  for (auto i = 0ul, e = numParam; i < e; ++i) {
    auto *arg = cs->getArgOperand(i);
    auto argVal = env.lookup(TaintValue(ctx, arg));
    if (argVal == TaintLattice::Unknown)
      break;
    callerVals.push_back(argVal);
  }
  return callerVals;
}

bool TransferFunction::updateParamTaintValue(
    const context::Context *newCtx, const Function *callee,
    const std::vector<TaintLattice> &argVals) {
  auto ret = false;
  const auto *paramItr = callee->arg_begin();
  for (auto argVal : argVals) {
    ret |=
        globalState.getEnv().weakUpdate(TaintValue(newCtx, &*paramItr), argVal);
    ++paramItr;
  }
  return ret;
}

void TransferFunction::evalInternalCall(const ProgramPoint &pp,
                                        const tpa::FunctionContext &fc,
                                        EvalResult &evalResult,
                                        bool callGraphUpdated) {
  auto *cs = llvm::dyn_cast<llvm::CallBase>(
      pp.getDefUseInstruction()->getInstruction());
  assert(cs);

  const auto *callee = fc.getFunction();
  auto numParam = callee->arg_size();
  if (cs->arg_size() < numParam)
    return;

  auto argSets = collectArgumentTaintValue(pp.getContext(), cs, numParam);
  if (argSets.size() < numParam)
    return;

  auto envChanged = updateParamTaintValue(fc.getContext(), callee, argSets) ||
                    callGraphUpdated;
  const auto *entryInst = globalState.getDefUseModule()
                              .getDefUseFunction(fc.getFunction())
                              .getEntryInst();

  evalEntry(pp, ProgramPoint(fc.getContext(), entryInst), evalResult,
            envChanged);
}

void TransferFunction::evalAtomic(const ProgramPoint &pp, EvalResult &result) {
  if (!localState)
    return;
  const auto *inst = pp.getDefUseInstruction()->getInstruction();
  const auto *rmw = dyn_cast<AtomicRMWInst>(inst);
  const auto *cx = dyn_cast<AtomicCmpXchgInst>(inst);
  const auto *ptr = rmw ? rmw->getPointerOperand() : cx->getPointerOperand();
  auto pointsTo =
      globalState.getPointerAnalysis().getPtsSet(pp.getContext(), ptr);
  if (pointsTo.empty())
    return;
  const auto oldValue = loadTaintFromPtsSet(pointsTo, *localState);
  auto &env = globalState.getEnv();
  const auto *replacement = rmw ? rmw->getValOperand() : cx->getNewValOperand();
  const auto replacementValue =
      env.lookup(TaintValue(pp.getContext(), replacement));
  auto returnedValue = oldValue;
  if (cx)
    // The aggregate represents both the old value and the comparison result.
    returnedValue = Lattice<TaintLattice>::merge(
        oldValue,
        env.lookup(TaintValue(pp.getContext(), cx->getCompareOperand())));
  if (returnedValue != TaintLattice::Unknown &&
      env.weakUpdate(TaintValue(pp.getContext(), inst), returnedValue))
    addTopLevelSuccessors(pp, result);

  result.setBaseStore(localState);
  if (replacementValue != TaintLattice::Unknown) {
    if (cx) {
      // The exchange may fail. Retain incoming contents as well as the new
      // value.
      weakUpdateStore(pointsTo, replacementValue, result);
    } else {
      const auto writtenValue =
          rmw->getOperation() == AtomicRMWInst::Xchg
              ? replacementValue
              : Lattice<TaintLattice>::merge(oldValue, replacementValue);
      const auto *object = *pointsTo.begin();
      if (pointsTo.size() == 1 && !object->isSummaryObject() &&
          !object->isSpecialObject())
        result.strongUpdateOverride(object, writtenValue);
      else
        weakUpdateStore(pointsTo, writtenValue, result);
    }
  }
  for (const auto *object : pointsTo)
    result.addDirectMemProp(pp, object);
}

void TransferFunction::evalCall(const ProgramPoint &pp,
                                EvalResult &evalResult) {
  const auto *inst = pp.getDefUseInstruction()->getInstruction();
  auto *cs = llvm::dyn_cast<llvm::CallBase>(inst);
  assert(cs);

  evalResult.setBaseStore(localState);

  const auto *ctx = pp.getContext();
  auto callees = resolveCallTargets(globalState.getPointerAnalysis(), *cs,
                                    globalState.getUnmodeledPolicy(), ctx);
  if (callees.empty()) {
    globalState.recordIgnoredUnresolvedCall(cs);
    evalIgnoredCall(pp, evalResult);
    return;
  }
  for (const auto *callTgt : callees) {
    if (callTgt->isDeclaration())
      evalExternalCall(pp, callTgt, evalResult);
    else {
      const auto *newCtx = context::pushContextForCall(ctx, inst);
      auto fc = FunctionContext(newCtx, callTgt);
      auto callGraphUpdated = globalState.getCallGraph().insertEdge(pp, fc);

      evalInternalCall(pp, fc, evalResult, callGraphUpdated);
      // A newly discovered caller must receive an already computed
      // return even when the callee's inputs did not change.
      if (callGraphUpdated)
        for (auto *exit : globalState.getDefUseModule()
                              .getDefUseFunction(callTgt)
                              .getMemoryExitInsts())
          evalResult.addTopLevelSuccessor(ProgramPoint(newCtx, exit));
    }
  }
}

void TransferFunction::applyReturn(const ProgramPoint &srcPP,
                                   const ProgramPoint &retSite,
                                   TaintLattice tVal, EvalResult &evalResult) {
  auto envChanged = false;
  if (tVal != TaintLattice::Unknown)
    envChanged = globalState.getEnv().weakUpdate(
        TaintValue(retSite.getContext(),
                   retSite.getDefUseInstruction()->getInstruction()),
        tVal);

  if (envChanged)
    addTopLevelSuccessors(retSite, evalResult);
  addMemBroadcast(srcPP, retSite, evalResult);
}

void TransferFunction::evalReturn(const ProgramPoint &pp,
                                  EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  const auto *duInst = pp.getDefUseInstruction();
  const auto *retInst = cast<ReturnInst>(duInst->getInstruction());

  const auto *fromFunc = duInst->getFunction();
  if (fromFunc->getName() == "main") {

    return;
  }

  evalResult.setBaseStore(localState);

  auto tVal = TaintLattice::Unknown;
  if (auto *retVal = retInst->getReturnValue())
    tVal = globalState.getEnv().lookup(TaintValue(ctx, retVal));

  auto returnTgts =
      globalState.getCallGraph().getCallers(FunctionContext(ctx, fromFunc));
  for (auto const &retSite : returnTgts)
    applyReturn(pp, retSite, tVal, evalResult);
}

void TransferFunction::evalResume(const ProgramPoint &pp, EvalResult &result) {
  result.setBaseStore(localState);
  const auto *function = pp.getDefUseInstruction()->getFunction();
  for (const auto &caller : globalState.getCallGraph().getCallers(
           FunctionContext(pp.getContext(), function)))
    applyReturn(pp, caller, TaintLattice::Unknown, result);
}

EvalResult TransferFunction::eval(const ProgramPoint &pp) {
  EvalResult evalResult;

  // errs() << "eval " << pp << "\n";

  const auto *duInst = pp.getDefUseInstruction();
  if (duInst->isEntryInstruction()) {
    evalResult.setBaseStore(localState);
    evalEntry(pp, pp, evalResult, true);
  } else {
    const auto *inst = duInst->getInstruction();
    assert(inst != nullptr);

    switch (inst->getOpcode()) {
    case Instruction::Alloca:
      evalAlloca(pp, evalResult);
      break;
    case Instruction::FNeg:
    case Instruction::Freeze:
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    case Instruction::IntToPtr:
    case Instruction::PtrToInt:
    case Instruction::BitCast:
    case Instruction::AddrSpaceCast:
    case Instruction::ExtractElement:
    case Instruction::ExtractValue:
      // Binary operators
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
    case Instruction::Add:
    case Instruction::FAdd:
    case Instruction::Sub:
    case Instruction::FSub:
    case Instruction::Mul:
    case Instruction::FMul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::FDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::FRem:
    case Instruction::ICmp:
    case Instruction::FCmp:
    case Instruction::ShuffleVector:
    case Instruction::InsertElement:
    case Instruction::InsertValue:
      // Ternary operators
    case Instruction::Select:
      // N-ary operators
    case Instruction::GetElementPtr:
    case Instruction::PHI:
      evalAllOperands(pp, evalResult);
      break;
    case Instruction::Store:
      evalStore(pp, evalResult);
      break;
    case Instruction::AtomicRMW:
    case Instruction::AtomicCmpXchg:
      evalAtomic(pp, evalResult);
      break;
    case Instruction::Load:
      evalLoad(pp, evalResult);
      break;
    // TODO: Add implicit flow detection for Br
    case Instruction::IndirectBr:
    case Instruction::Fence:
    case Instruction::Switch:
    case Instruction::Unreachable:
    case Instruction::Br:
      break;
    case Instruction::CallBr:
    case Instruction::Invoke:
    case Instruction::Call:
      evalCall(pp, evalResult);
      break;
    case Instruction::Ret:
      evalReturn(pp, evalResult);
      break;
    case Instruction::LandingPad:
      // Runtime handle/selector values are clean; accesses through extracted
      // exception pointers use the pointer backend's unknown-object model.
      evalAlloca(pp, evalResult);
      break;
    case Instruction::Resume:
      evalResume(pp, evalResult);
      break;
    case Instruction::VAArg: {
      errs() << "Insruction not handled :" << *inst << "\n";
      llvm_unreachable("Taint analysis failed");
    }
    }
  }

  return evalResult;
}

} // namespace tpa::taint
