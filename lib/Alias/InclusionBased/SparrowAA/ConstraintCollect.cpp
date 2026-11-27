/**
 * @file ConstraintCollect.cpp
 * @brief Constraint collection phase of Andersen's pointer analysis.
 *
 * This file implements the constraint collection phase that scans the program
 * and generates pointer constraints. It processes global variables, functions,
 * and instructions to build the constraint set that will be solved in later
 * phases.
 *
 * FIXME: The analysis does not use on-the-fly callgraph construction, but uses
 * a lightweight address-taken analysis to get the callee list. See the
 * implementation of Andersen::addConstraintForCall for details.
 *
 * @author rainoftime
 */
#include "Alias/InclusionBased/SparrowAA/Andersen.h"
#include "Alias/InclusionBased/SparrowAA/Log.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "andersen"

using namespace llvm;

STATISTIC(NumGlobalVariables, "Number of global variables");
STATISTIC(NumGlobalObjects, "Number of global objects created");
STATISTIC(NumAddrTakenFunctions, "Number of address-taken functions");
STATISTIC(NumReturnNodes, "Number of return nodes created");
STATISTIC(NumVarargNodes, "Number of vararg nodes created");
STATISTIC(NumAllocaNodes, "Number of alloca nodes created");
STATISTIC(NumObjectNodes, "Number of object nodes created");
STATISTIC(NumDirectCalls, "Number of direct function calls");
STATISTIC(NumIndirectCalls, "Number of indirect function calls");
STATISTIC(NumExternalLibCalls, "Number of external library calls");
STATISTIC(NumUnresolvedLibCalls, "Number of unresolved library calls");
STATISTIC(NumCallSites, "Number of call sites processed");
STATISTIC(NumFunctions, "Number of functions analyzed");
STATISTIC(NumPointerInstructions, "Number of pointer instructions processed");

/**
 * @brief Collect constraints from the entire module.
 *
 * This stage scans the program, adding a constraint to the constraints list
 * for each instruction that induces a constraint, and setting up the initial
 * points-to graph. Initializes universal and null pointer constraints, then
 * processes globals and all functions.
 *
 * @param M The module to collect constraints from
 */
void Andersen::collectConstraints(const Module &M) {
  LOG_INFO("collectConstraints: Starting, nodeFactory has {} nodes",
           nodeFactory.getNumNodes());

  // First, the universal ptr points to universal obj, and the universal obj
  // points to itself
  constraints.emplace_back(AndersConstraint::ADDR_OF,
                           nodeFactory.getUniversalPtrNode(),
                           nodeFactory.getUniversalObjNode());
  constraints.emplace_back(AndersConstraint::STORE,
                           nodeFactory.getUniversalObjNode(),
                           nodeFactory.getUniversalObjNode());

  // Next, the null pointer points to the null object.
  constraints.emplace_back(AndersConstraint::ADDR_OF,
                           nodeFactory.getNullPtrNode(),
                           nodeFactory.getNullObjectNode());

  LOG_INFO("collectConstraints: Added {} initial constraints",
           constraints.size());

  // Next, add any constraints on global variables. Associate the address of the
  // global object as pointing to the memory for the global: &G = <G memory>
  // Use initialCtx for globals to ensure consistency with function analysis
  collectConstraintsForGlobals(M, initialCtx);

  LOG_INFO(
      "collectConstraints: After globals, have {} constraints and {} nodes",
      constraints.size(), nodeFactory.getNumNodes());

  // Schedule every defined function with the initial context, then process
  // all scheduled functions iteratively via a worklist.  The worklist avoids
  // depth-first recursion through addConstraintForCall -> scheduleFunction,
  // which could overflow the native call stack on programs with dense
  // indirect-call graphs.
  pendingFunctions.clear();
  for (auto const &f : M) {
    if (f.isDeclaration() || f.isIntrinsic())
      continue;
    scheduleFunction(&f, initialCtx);
  }
  while (!pendingFunctions.empty()) {
    auto [f, ctx] = pendingFunctions.front();
    pendingFunctions.pop_front();
    collectConstraintsForFunction(f, ctx);
  }
}

/**
 * @brief Collect constraints for a single function in a given context.
 *
 * Creates function-scoped nodes (return, vararg, arguments) and processes
 * all instructions in the function to generate constraints. Uses a visited
 * set to avoid reprocessing the same function-context pair.
 *
 * @param f The function to process
 * @param ctx The context key for this function instance
 */

void Andersen::scheduleFunction(const llvm::Function *f,
                                AndersNodeFactory::CtxKey ctx) {
  FunctionContextKey key{f, ctx};
  if (!visitedFunctions.insert(key).second)
    return;

  ++NumFunctions;

  // Per-context function-scoped nodes (needed by addConstraintForCall for
  // argument/return constraints).
  if (f->getFunctionType()->getReturnType()->isPointerTy()) {
    nodeFactory.createReturnNode(f, ctx);
    ++NumReturnNodes;
  }
  if (f->getFunctionType()->isVarArg()) {
    nodeFactory.createVarargNode(f, ctx);
    ++NumVarargNodes;
  }
  for (Function::const_arg_iterator itr = f->arg_begin(), ite = f->arg_end();
       itr != ite; ++itr) {
    if (isa<PointerType>(itr->getType()))
      nodeFactory.createValueNode(&*itr, ctx);
  }

  // Create a value node for each pointer-typed instruction (first pass).
  for (const_inst_iterator itr = inst_begin(*f), ite = inst_end(*f); itr != ite;
       ++itr) {
    const auto *inst = &*itr.getInstructionIterator();
    if (inst->getType()->isPointerTy()) {
      nodeFactory.createValueNode(inst, ctx);
      ++NumPointerInstructions;
    }
  }

  // Defer instruction processing to the worklist so that we never recurse
  // through addConstraintForCall -> collectConstraintsForFunction -> ...
  pendingFunctions.emplace_back(f, ctx);
}


void Andersen::collectConstraintsForFunction(const llvm::Function *f,
                                             AndersNodeFactory::CtxKey ctx) {
  // Process each instruction (second pass).  Value and function-scoped nodes
  // have already been created by a prior call to scheduleFunction().
  for (const_inst_iterator itr = inst_begin(*f), ite = inst_end(*f); itr != ite;
       ++itr) {
    const auto *inst = &*itr.getInstructionIterator();
    collectConstraintsForInstruction(inst, ctx);
  }
}

/**
 * @brief Collect constraints for global variables and address-taken functions.
 *
 * Creates value and object nodes for each global variable and generates
 * ADDR_OF constraints. Also handles address-taken functions by creating
 * function pointer nodes.
 *
 * @param M The module containing the globals
 * @param ctx The context key for global processing
 */
void Andersen::collectConstraintsForGlobals(const Module &M,
                                            AndersNodeFactory::CtxKey ctx) {
  varArgAddrTakenFunctions.clear();
  fixedArgAddrTakenFunctions.clear();

  // Create a pointer and an object for each global variable
  for (auto const &globalVal : M.globals()) {
    NodeIndex gVal = nodeFactory.createValueNode(&globalVal, ctx);
    NodeIndex gObj = nodeFactory.createObjectNode(&globalVal, ctx);
    constraints.emplace_back(AndersConstraint::ADDR_OF, gVal, gObj);
    ++NumGlobalVariables;
    ++NumGlobalObjects;
  }

  // Functions and function pointers are also considered global.
  // Cache address-taken function signatures once so indirect-call resolution
  // does not rescan M or re-walk every function's use-list per call site.
  for (auto const &f : M) {
    // If f is an addr-taken function, create a pointer and an object for it.
    // These are global (context-independent) because the function's address
    // is a compile-time constant.
    if (f.hasAddressTaken()) {
      NodeIndex fVal = nodeFactory.createValueNode(&f, ctx);
      NodeIndex fObj = nodeFactory.createObjectNode(&f, ctx);
      constraints.emplace_back(AndersConstraint::ADDR_OF, fVal, fObj);
      ++NumAddrTakenFunctions;

      const FunctionType *fty = f.getFunctionType();
      AddrTakenFunctionInfo info;
      info.func = &f;
      info.retIsPtr = fty->getReturnType()->isPointerTy();
      info.numParams = fty->getNumParams();
      uint64_t mask = 0;
      unsigned limit = std::min(info.numParams, 64u);
      for (unsigned i = 0; i < limit; ++i) {
        if (fty->getParamType(i)->isPointerTy())
          mask |= (uint64_t{1} << i);
      }
      info.paramPtrMask = mask;
      if (fty->isVarArg())
        varArgAddrTakenFunctions.push_back(info);
      else
        fixedArgAddrTakenFunctions[info.numParams].push_back(info);
    }
  }

  // Init globals here since an initializer may refer to a global var/func below
  // it
  for (auto const &globalVal : M.globals()) {
    NodeIndex gObj = nodeFactory.getObjectNodeFor(&globalVal, ctx);
    assert(gObj != AndersNodeFactory::InvalidIndex &&
           "Cannot find global object!");

    if (globalVal.hasDefinitiveInitializer()) {
      addGlobalInitializerConstraints(gObj, globalVal.getInitializer(), ctx);
    } else {
      // If it doesn't have an initializer (i.e. it's defined in another
      // translation unit), it points to the universal set.
      constraints.emplace_back(AndersConstraint::COPY, gObj,
                               nodeFactory.getUniversalObjNode());
    }
  }
}

void Andersen::addGlobalInitializerConstraints(NodeIndex objNode,
                                               const Constant *c,
                                               AndersNodeFactory::CtxKey ctx) {
  if (isa<UndefValue>(c) || isa<ConstantAggregateZero>(c)) {
    // Undefined or zero-initialized aggregate carries no non-null pointers.
    return;
  }
  if (isa<ConstantVector>(c)) {
    // VectorType::isSingleValueType() is true in LLVM, so check ConstantVector
    // before isSingleValueType() to avoid dropping pointer elements.
    for (unsigned i = 0, e = c->getNumOperands(); i != e; ++i)
      addGlobalInitializerConstraints(objNode, cast<Constant>(c->getOperand(i)),
                                      ctx);
    return;
  }
  if (c->getType()->isSingleValueType()) {
    if (isa<PointerType>(c->getType())) {
      NodeIndex rhsNode = nodeFactory.getObjectNodeForConstant(c, ctx);
      if (rhsNode == AndersNodeFactory::InvalidIndex)
        rhsNode = nodeFactory.getUniversalObjNode();
      constraints.emplace_back(AndersConstraint::ADDR_OF, objNode, rhsNode);
    }
  } else if (c->isNullValue()) {
    // Scalar null pointer.
    constraints.emplace_back(AndersConstraint::COPY, objNode,
                             nodeFactory.getNullObjectNode());
  } else if (isa<ConstantArray>(c) || isa<ConstantDataSequential>(c) ||
             isa<ConstantStruct>(c)) {
    // Since we are doing field-insensitive analysis, all objects in the
    // array/struct are pointed-to by the 1st-field pointer.
    for (unsigned i = 0, e = c->getNumOperands(); i != e; ++i)
      addGlobalInitializerConstraints(objNode, cast<Constant>(c->getOperand(i)),
                                      ctx);
  }
}

/**
 * @brief Collect constraints for a single instruction.
 *
 * Dispatches to instruction-specific constraint collectors based on opcode.
 * Handles Alloca, Load, Store, GEP, BitCast, PHI, Select, and Call
 * instructions.
 *
 * @param inst The instruction to process
 * @param ctx The context key for this instruction
 */
void Andersen::collectConstraintsForInstruction(const Instruction *inst,
                                                AndersNodeFactory::CtxKey ctx) {
  auto getValueNodeOrUniversal = [&](const Value *val) -> NodeIndex {
    NodeIndex idx = nodeFactory.getValueNodeFor(val, ctx);
    return idx != AndersNodeFactory::InvalidIndex
               ? idx
               : nodeFactory.getUniversalPtrNode();
  };

  switch (inst->getOpcode()) {
  case Instruction::Alloca: {
    NodeIndex valNode = nodeFactory.getValueNodeFor(inst, ctx);
    if (valNode == AndersNodeFactory::InvalidIndex)
      break;
    NodeIndex objNode = nodeFactory.createObjectNode(inst, ctx);
    constraints.emplace_back(AndersConstraint::ADDR_OF, valNode, objNode);
    ++NumAllocaNodes;
    ++NumObjectNodes;
    break;
  }
  case Instruction::Call:
  case Instruction::Invoke: {
    if (const CallBase *cs = dyn_cast<CallBase>(inst)) {
      addConstraintForCall(cs, ctx);
      ++NumCallSites;
    }
    break;
  }
  case Instruction::Ret: {
    if (inst->getNumOperands() > 0 &&
        inst->getOperand(0)->getType()->isPointerTy()) {
      NodeIndex retIndex =
          nodeFactory.getReturnNodeFor(inst->getParent()->getParent(), ctx);
      if (retIndex == AndersNodeFactory::InvalidIndex)
        break;
      NodeIndex valIndex = getValueNodeOrUniversal(inst->getOperand(0));
      constraints.emplace_back(AndersConstraint::COPY, retIndex, valIndex);
    }
    break;
  }
  case Instruction::Load: {
    if (inst->getType()->isPointerTy()) {
      NodeIndex opIndex = getValueNodeOrUniversal(inst->getOperand(0));
      NodeIndex valIndex = nodeFactory.getValueNodeFor(inst, ctx);
      if (valIndex != AndersNodeFactory::InvalidIndex)
        constraints.emplace_back(AndersConstraint::LOAD, valIndex, opIndex);
    }
    break;
  }
  case Instruction::Store: {
    if (inst->getOperand(0)->getType()->isPointerTy()) {
      NodeIndex srcIndex = getValueNodeOrUniversal(inst->getOperand(0));
      NodeIndex dstIndex = getValueNodeOrUniversal(inst->getOperand(1));
      constraints.emplace_back(AndersConstraint::STORE, dstIndex, srcIndex);
    }
    break;
  }
  case Instruction::GetElementPtr: {
    // Vector GEPs return a vector of pointers rather than a scalar pointer.
    if (inst->getType()->isPointerTy()) {
      // P1 = getelementptr P2, ... --> <Copy/P1/P2>
      NodeIndex srcIndex = getValueNodeOrUniversal(inst->getOperand(0));
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      if (dstIndex != AndersNodeFactory::InvalidIndex)
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
    }
    break;
  }
  case Instruction::PHI: {
    if (inst->getType()->isPointerTy()) {
      const PHINode *phiInst = cast<PHINode>(inst);
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(phiInst, ctx);
      if (dstIndex == AndersNodeFactory::InvalidIndex)
        break;
      for (unsigned i = 0, e = phiInst->getNumIncomingValues(); i != e; ++i) {
        NodeIndex srcIndex =
            getValueNodeOrUniversal(phiInst->getIncomingValue(i));
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      }
    }
    break;
  }
  case Instruction::BitCast:
  case Instruction::AddrSpaceCast: {
    if (inst->getType()->isPointerTy()) {
      NodeIndex srcIndex = getValueNodeOrUniversal(inst->getOperand(0));
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      if (dstIndex != AndersNodeFactory::InvalidIndex)
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
    }
    break;
  }
  case Instruction::Freeze: {
    if (inst->getType()->isPointerTy()) {
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      if (dstIndex == AndersNodeFactory::InvalidIndex)
        break;
      const Value *op = inst->getOperand(0);
      // Freezing undef/poison can yield any pointer; freezing a concrete
      // pointer preserves its value.
      NodeIndex srcIndex = isa<UndefValue>(op)
                               ? nodeFactory.getUniversalPtrNode()
                               : getValueNodeOrUniversal(op);
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
    }
    break;
  }
  case Instruction::IntToPtr: {
    // Vector inttoptr produces a vector of pointers rather than a scalar
    // pointer.
    if (!inst->getType()->isPointerTy())
      break;

    // Get the node index for dst
    NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
    if (dstIndex == AndersNodeFactory::InvalidIndex)
      break;

    // We use pattern matching to look for a matching ptrtoint
    Value *op = inst->getOperand(0);

    // Pointer copy: Y = inttoptr (ptrtoint X)
    Value *srcValue = nullptr;
    if (PatternMatch::match(
            op, PatternMatch::m_PtrToInt(PatternMatch::m_Value(srcValue)))) {
      NodeIndex srcIndex = getValueNodeOrUniversal(srcValue);
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      break;
    }

    // Pointer arithmetic: Y = inttoptr (ptrtoint (X) + offset) or commuted
    if (PatternMatch::match(
            op, PatternMatch::m_c_Add(
                    PatternMatch::m_PtrToInt(PatternMatch::m_Value(srcValue)),
                    PatternMatch::m_Value()))) {
      NodeIndex srcIndex = getValueNodeOrUniversal(srcValue);
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      break;
    }

    // Otherwise, we really don't know what dst points to
    constraints.emplace_back(AndersConstraint::COPY, dstIndex,
                             nodeFactory.getUniversalPtrNode());

    break;
  }
  case Instruction::Select: {
    if (inst->getType()->isPointerTy()) {
      NodeIndex srcIndex1 = getValueNodeOrUniversal(inst->getOperand(1));
      NodeIndex srcIndex2 = getValueNodeOrUniversal(inst->getOperand(2));
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      if (dstIndex != AndersNodeFactory::InvalidIndex) {
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex1);
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex2);
      }
    }
    break;
  }
  case Instruction::VAArg: {
    if (inst->getType()->isPointerTy()) {
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      NodeIndex vaIndex =
          nodeFactory.getVarargNodeFor(inst->getParent()->getParent(), ctx);
      if (dstIndex != AndersNodeFactory::InvalidIndex) {
        NodeIndex srcIndex = vaIndex != AndersNodeFactory::InvalidIndex
                                 ? vaIndex
                                 : nodeFactory.getUniversalPtrNode();
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      }
    }
    break;
  }
  case Instruction::ExtractValue:
  case Instruction::ExtractElement: {
    // ExtractValue / ExtractElement extracts a value from an aggregate or
    // vector.
    if (inst->getType()->isPointerTy()) {
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      if (dstIndex == AndersNodeFactory::InvalidIndex)
        break;

      Value *aggOperand = inst->getOperand(0);
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(aggOperand, ctx);
      if (srcIndex != AndersNodeFactory::InvalidIndex) {
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      } else {
        constraints.emplace_back(AndersConstraint::COPY, dstIndex,
                                 nodeFactory.getUniversalPtrNode());
      }
    }
    break;
  }
  case Instruction::InsertValue:
  case Instruction::InsertElement: {
    Value *insertedVal = inst->getOperand(1);
    if (insertedVal->getType()->isPointerTy()) {
      NodeIndex dstIndex = nodeFactory.createValueNode(inst, ctx);

      Value *aggOperand = inst->getOperand(0);
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(aggOperand, ctx);
      if (srcIndex != AndersNodeFactory::InvalidIndex) {
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      }

      NodeIndex insertedIndex = getValueNodeOrUniversal(insertedVal);
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, insertedIndex);
    }
    break;
  }
  case Instruction::ShuffleVector: {
    if (const auto *vty = dyn_cast<VectorType>(inst->getType())) {
      if (vty->getElementType()->isPointerTy()) {
        NodeIndex src0 = nodeFactory.getValueNodeFor(inst->getOperand(0), ctx);
        NodeIndex src1 = nodeFactory.getValueNodeFor(inst->getOperand(1), ctx);
        if (src0 != AndersNodeFactory::InvalidIndex ||
            src1 != AndersNodeFactory::InvalidIndex) {
          NodeIndex dstIndex = nodeFactory.createValueNode(inst, ctx);
          if (src0 != AndersNodeFactory::InvalidIndex)
            constraints.emplace_back(AndersConstraint::COPY, dstIndex, src0);
          if (src1 != AndersNodeFactory::InvalidIndex)
            constraints.emplace_back(AndersConstraint::COPY, dstIndex, src1);
        }
      }
    }
    break;
  }
  // We have no intention to support exception-handling in the near future. Just
  // ignore EH-related instructions instead of crashing.
  case Instruction::LandingPad:
  case Instruction::Resume: {
    break;
  }
  // Atomic instructions can be modeled conservatively by their non-atomic
  // counterparts.
  case Instruction::AtomicRMW: {
    const auto *ar = cast<AtomicRMWInst>(inst);

    // Load-like effect: the result is the old value in memory.
    if (ar->getType()->isPointerTy()) {
      NodeIndex ptrIndex = getValueNodeOrUniversal(ar->getPointerOperand());
      NodeIndex resIndex = nodeFactory.getValueNodeFor(ar, ctx);
      if (resIndex != AndersNodeFactory::InvalidIndex)
        constraints.emplace_back(AndersConstraint::LOAD, resIndex, ptrIndex);
    }

    // Store-like effect: the new value is written back to memory.
    const Value *valOp = ar->getValOperand();
    if (valOp->getType()->isPointerTy()) {
      NodeIndex srcIndex = getValueNodeOrUniversal(valOp);
      NodeIndex dstIndex = getValueNodeOrUniversal(ar->getPointerOperand());
      constraints.emplace_back(AndersConstraint::STORE, dstIndex, srcIndex);
    }
    break;
  }
  case Instruction::AtomicCmpXchg: {
    const auto *cx = cast<AtomicCmpXchgInst>(inst);

    // Store-like effect: if the exchanged-in value is a pointer, it may be
    // written to memory.
    const Value *newVal = cx->getNewValOperand();
    if (newVal->getType()->isPointerTy()) {
      NodeIndex srcIndex = getValueNodeOrUniversal(newVal);
      NodeIndex dstIndex = getValueNodeOrUniversal(cx->getPointerOperand());
      constraints.emplace_back(AndersConstraint::STORE, dstIndex, srcIndex);
    }

    const Value *cmpVal = cx->getCompareOperand();
    if (cmpVal->getType()->isPointerTy()) {
      NodeIndex ptrIndex = getValueNodeOrUniversal(cx->getPointerOperand());
      // Create a value node for the aggregate result so that a later
      // ExtractValue can copy the old pointer out of it.
      NodeIndex resIndex = nodeFactory.createValueNode(inst, ctx);
      constraints.emplace_back(AndersConstraint::LOAD, resIndex, ptrIndex);
    }
    break;
  }
  default: {
    if (inst->getType()->isPointerTy()) {
      LOG_WARN("pointer-related inst not handled precisely (falling back to "
               "universal ptr): {}",
               *inst);
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      if (dstIndex != AndersNodeFactory::InvalidIndex)
        constraints.emplace_back(AndersConstraint::COPY, dstIndex,
                                 nodeFactory.getUniversalPtrNode());
    }
    break;
  }
  }
}

// There are two types of constraints to add for a function call:
// - ValueNode(callsite) = ReturnNode(call target)
// - ValueNode(formal arg) = ValueNode(actual arg)
/**
 * @brief Add constraints for a function call instruction.
 *
 * Handles both direct and indirect calls. For direct calls, processes the
 * called function. For indirect calls, uses address-taken analysis to find
 * potential callees. Spawns new contexts for context-sensitive analysis.
 *
 * @param cs The call site instruction
 * @param callerCtx The context of the calling function
 */
void Andersen::addConstraintForCall(const llvm::CallBase *cs,
                                    AndersNodeFactory::CtxKey callerCtx) {
  auto polluteCallSite = [&]() {
    if (cs->getType()->isPointerTy()) {
      NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
      if (retIndex != AndersNodeFactory::InvalidIndex)
        constraints.emplace_back(AndersConstraint::COPY, retIndex,
                                 nodeFactory.getUniversalPtrNode());
    }
    for (unsigned i = 0; i < cs->arg_size(); ++i) {
      Value *argVal = cs->getArgOperand(i);
      if (argVal->getType()->isPointerTy()) {
        NodeIndex argIndex = nodeFactory.getValueNodeFor(argVal, callerCtx);
        if (argIndex != AndersNodeFactory::InvalidIndex)
          constraints.emplace_back(AndersConstraint::COPY, argIndex,
                                   nodeFactory.getUniversalPtrNode());
      }
    }
  };

  if (const Function *f = cs->getCalledFunction()) // Direct call
  {
    ++NumDirectCalls;
    if (f->isDeclaration() || f->isIntrinsic()) // External library call
    {
      ++NumExternalLibCalls;
      // Handle libraries separately
      if (addConstraintForExternalLibrary(cs, f, callerCtx)) {
        return;
      } else { // Unresolved library call: ruin everything!
        ++NumUnresolvedLibCalls;
        polluteCallSite();
      }
    } else // Non-external function call
    {
      AndersNodeFactory::CtxKey calleeCtx =
          evolveContextForCallee(callerCtx, cs, f);
      scheduleFunction(f, calleeCtx);

      if (cs->getType()->isPointerTy()) {
        NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
        NodeIndex fRetIndex = nodeFactory.getReturnNodeFor(f, calleeCtx);
        if (retIndex != AndersNodeFactory::InvalidIndex &&
            fRetIndex != AndersNodeFactory::InvalidIndex)
          constraints.emplace_back(AndersConstraint::COPY, retIndex, fRetIndex);
      }
      // The argument constraints
      addArgumentConstraintForCall(cs, f, callerCtx, calleeCtx);
    }
  } else // Indirect call
  {
    ++NumIndirectCalls;
    const FunctionType *callSiteFTy = cs->getFunctionType();
    bool csRetIsPtr =
        callSiteFTy ? callSiteFTy->getReturnType()->isPointerTy() : false;
    unsigned csNumParams = callSiteFTy ? callSiteFTy->getNumParams() : 0;
    uint64_t csParamPtrMask = 0;
    if (callSiteFTy) {
      unsigned limit = std::min(csNumParams, 64u);
      for (unsigned i = 0; i < limit; ++i) {
        if (callSiteFTy->getParamType(i)->isPointerTy())
          csParamPtrMask |= (uint64_t{1} << i);
      }
    }

    auto isTypeCompatible = [&](const AddrTakenFunctionInfo &info) -> bool {
      if (!callSiteFTy)
        return true; // Cannot determine type; be conservative.
      if (csRetIsPtr != info.retIsPtr)
        return false;

      unsigned numFixed = std::min(csNumParams, info.numParams);
      unsigned maskBits = std::min(numFixed, 64u);
      if (maskBits > 0) {
        uint64_t mask = (maskBits == 64u) ? ~uint64_t{0}
                                          : ((uint64_t{1} << maskBits) - 1u);
        if ((csParamPtrMask & mask) != (info.paramPtrMask & mask))
          return false;
      }
      if (numFixed > 64u) {
        const FunctionType *calleeFTy = info.func->getFunctionType();
        for (unsigned i = 64u; i < numFixed; ++i) {
          if (callSiteFTy->getParamType(i)->isPointerTy() !=
              calleeFTy->getParamType(i)->isPointerTy())
            return false;
        }
      }
      return true;
    };

    bool foundAnyCallee = false;
    bool pollutedIndirect = false;
    auto processCandidate = [&](const AddrTakenFunctionInfo &info) {
      if (!isTypeCompatible(info))
        return;

      const Function &f = *info.func;
      foundAnyCallee = true;

      if (f.isDeclaration() || f.isIntrinsic()) // External library call
      {
        if (addConstraintForExternalLibrary(cs, &f, callerCtx))
          return;
        if (!pollutedIndirect) {
          pollutedIndirect = true;
          polluteCallSite();
        }
      } else {
        AndersNodeFactory::CtxKey calleeCtx =
            evolveContextForCallee(callerCtx, cs, &f);
        scheduleFunction(&f, calleeCtx);

        // Connect the callee's return node to the call-site value node.
        if (cs->getType()->isPointerTy()) {
          NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
          NodeIndex fRetIndex = nodeFactory.getReturnNodeFor(&f, calleeCtx);
          if (retIndex != AndersNodeFactory::InvalidIndex &&
              fRetIndex != AndersNodeFactory::InvalidIndex)
            constraints.emplace_back(AndersConstraint::COPY, retIndex,
                                     fRetIndex);
        }

        addArgumentConstraintForCall(cs, &f, callerCtx, calleeCtx);
      }
    };

    auto fixedIt = fixedArgAddrTakenFunctions.find(cs->arg_size());
    if (fixedIt != fixedArgAddrTakenFunctions.end()) {
      for (const auto &info : fixedIt->second)
        processCandidate(info);
    }
    for (const auto &info : varArgAddrTakenFunctions)
      processCandidate(info);

    // If no callee was found at all, fall back conservatively.
    if (!foundAnyCallee)
      polluteCallSite();
  }
}

void Andersen::addArgumentConstraintForCall(
    const llvm::CallBase *cs, const Function *f,
    AndersNodeFactory::CtxKey callerCtx, AndersNodeFactory::CtxKey calleeCtx) {
  Function::const_arg_iterator fItr = f->arg_begin();
  unsigned argIdx = 0;

  while (fItr != f->arg_end() && argIdx < cs->arg_size()) {
    const Argument *formal = &*fItr;
    const Value *actual = cs->getArgOperand(argIdx);

    if (formal->getType()->isPointerTy()) {
      NodeIndex fIndex = nodeFactory.getValueNodeFor(formal, calleeCtx);
      if (fIndex != AndersNodeFactory::InvalidIndex) {
        if (actual->getType()->isPointerTy()) {
          NodeIndex aIndex = nodeFactory.getValueNodeFor(actual, callerCtx);
          if (aIndex == AndersNodeFactory::InvalidIndex)
            aIndex = nodeFactory.getUniversalPtrNode();
          constraints.emplace_back(AndersConstraint::COPY, fIndex, aIndex);
        } else {
          constraints.emplace_back(AndersConstraint::COPY, fIndex,
                                   nodeFactory.getUniversalPtrNode());
        }
      }
    }

    ++fItr;
    ++argIdx;
  }

  // Copy all pointers passed through the varargs section to the varargs node
  if (f->getFunctionType()->isVarArg()) {
    NodeIndex vaIndex = nodeFactory.getVarargNodeFor(f, calleeCtx);
    if (vaIndex != AndersNodeFactory::InvalidIndex) {
      while (argIdx < cs->arg_size()) {
        const Value *actual = cs->getArgOperand(argIdx);
        if (actual->getType()->isPointerTy()) {
          NodeIndex aIndex = nodeFactory.getValueNodeFor(actual, callerCtx);
          if (aIndex == AndersNodeFactory::InvalidIndex)
            aIndex = nodeFactory.getUniversalPtrNode();
          constraints.emplace_back(AndersConstraint::COPY, vaIndex, aIndex);
        }

        ++argIdx;
      }
    }
  }
}

// The implementation of addConstraintForExternalLibrary is in
// ExternalLibrary.cpp so we remove the duplicate implementation here
