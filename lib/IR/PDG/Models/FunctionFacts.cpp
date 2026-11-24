#include "IR/PDG/Models/FunctionFacts.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Models/ValueFacts.h"

#include <map>
#include <set>

using namespace llvm;
namespace pdg {

static bool blockReaches(const BasicBlock *from, const BasicBlock *to) {
  SmallVector<const BasicBlock *, 16> todo{from};
  SmallPtrSet<const BasicBlock *, 32> seen;
  while (!todo.empty()) {
    const BasicBlock *block = todo.pop_back_val();
    if (block == to)
      return true;
    if (!seen.insert(block).second)
      continue;
    for (const BasicBlock *successor : successors(block))
      todo.push_back(successor);
  }
  return false;
}

struct FunctionFacts::Impl {
  Function &function;
  TargetLibraryInfoImpl tli_impl;
  TargetLibraryInfo tli;
  mutable AssumptionCache assumptions;
  DominatorTree dominators;
  LoopInfo loops;
  BasicAAResult basic_aa;
  mutable AAResults aa;
  mutable ScalarEvolution evolution;
  mutable std::unique_ptr<MemorySSA> memory_ssa;
  std::map<const Value *, APInt> constant_arguments;
  mutable std::map<const Value *, const Value *> pointer_origins;

  const Value *pointerOrigin(const Value *value, unsigned depth) const {
    if (!value->getType()->isPointerTy() || !depth)
      return value;
    value = value->stripPointerCastsAndAliases();
    auto known = pointer_origins.find(value);
    if (known != pointer_origins.end())
      return known->second;
    const auto *load = dyn_cast<LoadInst>(value);
    if (!load || load->getFunction() != &function || load->isAtomic() ||
        load->isVolatile())
      return value;
    if (!memory_ssa)
      memory_ssa = std::make_unique<MemorySSA>(
          function, &aa, const_cast<DominatorTree *>(&dominators));
    const Value *result = nullptr;
    bool ambiguous = false;
    SmallPtrSet<const MemoryAccess *, 16> seen;
    SmallVector<MemoryAccess *, 16> pending;
    pending.push_back(memory_ssa->getWalker()->getClobberingMemoryAccess(load));
    while (!pending.empty()) {
      auto *access = pending.pop_back_val();
      if (!access) {
        ambiguous = true;
        break;
      }
      if (!seen.insert(access).second)
        continue;
      if (const auto *phi = dyn_cast<MemoryPhi>(access)) {
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
          pending.push_back(phi->getIncomingValue(i));
        continue;
      }
      const auto *definition = dyn_cast<MemoryDef>(access);
      const auto *store =
          definition ? dyn_cast_or_null<StoreInst>(definition->getMemoryInst())
                     : nullptr;
      if (!store || store->isAtomic() || store->isVolatile() ||
          store->getValueOperand()->getType() != load->getType() ||
          aa.alias(MemoryLocation::get(store), MemoryLocation::get(load)) !=
              AliasResult::MustAlias) {
        ambiguous = true;
        break;
      }
      const Value *incoming =
          pointerOrigin(store->getValueOperand(), depth - 1);
      if (result && result != incoming) {
        ambiguous = true;
        break;
      }
      result = incoming;
    }
    result = !ambiguous && result ? result : value;
    pointer_origins.emplace(value, result);
    return result;
  }

  explicit Impl(Function &f)
      : function(f), tli_impl(Triple(f.getParent()->getTargetTriple())),
        tli(tli_impl), assumptions(f), dominators(f), loops(dominators),
        basic_aa(f.getParent()->getDataLayout(), f, tli, assumptions,
                 &dominators),
        aa(tli), evolution(f, tli, assumptions, dominators, loops) {
    aa.addAAResult(basic_aa);
  }

  bool equivalent(const Value *left, const Value *right, unsigned depth) const {
    left = left->stripPointerCasts();
    right = right->stripPointerCasts();
    if (isa<UndefValue>(left) || isa<UndefValue>(right))
      return false;
    if (left == right)
      return true;
    if (depth == 0 || left->getType() != right->getType())
      return false;
    const auto *a = dyn_cast<Instruction>(left),
               *b = dyn_cast<Instruction>(right);
    if (!a || !b || a->getFunction() != &function ||
        b->getFunction() != &function)
      return false;
    // Allocation sites create distinct storage even when their types, sizes
    // and alignments match. Distinct freeze results may also choose different
    // values from the same undefined input.
    if (isa<AllocaInst>(a) || isa<AllocaInst>(b) || isa<FreezeInst>(a) ||
        isa<FreezeInst>(b))
      return false;
    if (const auto *la = dyn_cast<LoadInst>(a)) {
      const auto *lb = dyn_cast<LoadInst>(b);
      if (!lb || la->isVolatile() || lb->isVolatile() || la->isAtomic() ||
          lb->isAtomic() ||
          !equivalent(la->getPointerOperand(), lb->getPointerOperand(),
                      depth - 1))
        return false;
      if (la->getParent() != lb->getParent()) {
        if (!memory_ssa)
          memory_ssa = std::make_unique<MemorySSA>(
              function, &aa, const_cast<DominatorTree *>(&dominators));
        auto *walker = memory_ssa->getWalker();
        return walker->getClobberingMemoryAccess(la) ==
               walker->getClobberingMemoryAccess(lb);
      }
      const Instruction *first = la, *last = lb;
      if (!first->comesBefore(last))
        std::swap(first, last);
      MemoryLocation location = MemoryLocation::get(la);
      for (const Instruction *i = first->getNextNode(); i != last;
           i = i->getNextNode()) {
        if (isa<FenceInst>(i) || i->isAtomic())
          return false;
        if (!i->mayWriteToMemory())
          continue;
        if (const auto *call = dyn_cast<CallBase>(i))
          if (const Function *target = ValueFacts::callee(*call))
            if (LibraryModels::readsOnly(*target))
              continue;
        if (isModSet(aa.getModRefInfo(i, location)))
          return false;
      }
      return true;
    }
    if (isa<PHINode>(a) || isa<CallBase>(a) || a->mayReadOrWriteMemory() ||
        !a->isSameOperationAs(b) || a->getNumOperands() != b->getNumOperands())
      return false;
    for (unsigned i = 0; i < a->getNumOperands(); ++i)
      if (!equivalent(a->getOperand(i), b->getOperand(i), depth - 1))
        return false;
    return true;
  }

  ConstantInt *atBackedge(const Value *value, const Loop &loop,
                          unsigned depth) const {
    if (const auto *c = dyn_cast<ConstantInt>(value))
      return const_cast<ConstantInt *>(c);
    if (!depth)
      return nullptr;
    const auto *inst = dyn_cast<Instruction>(value);
    if (!inst)
      return nullptr;
    if (const auto *phi = dyn_cast<PHINode>(inst)) {
      ConstantInt *result = nullptr;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        if (!loop.contains(phi->getIncomingBlock(i)))
          continue;
        ConstantInt *incoming =
            atBackedge(phi->getIncomingValue(i), loop, depth - 1);
        if (!incoming || (result && incoming != result))
          return nullptr;
        result = incoming;
      }
      return result;
    }
    if (const auto *load = dyn_cast<LoadInst>(inst)) {
      if (load->isVolatile() || load->isAtomic())
        return nullptr;
      ConstantInt *result = nullptr;
      bool mandatory = false;
      SmallVector<BasicBlock *, 8> latches;
      loop.getLoopLatches(latches);
      for (const BasicBlock *block : loop.blocks())
        for (const Instruction &other : *block) {
          if (!other.mayWriteToMemory())
            continue;
          if (const auto *store = dyn_cast<StoreInst>(&other)) {
            if (equivalent(store->getPointerOperand(),
                           load->getPointerOperand(), 16)) {
              auto *constant = dyn_cast<ConstantInt>(store->getValueOperand());
              if (!constant || store->isVolatile() || store->isAtomic() ||
                  (result && result != constant))
                return nullptr;
              result = const_cast<ConstantInt *>(constant);
              bool dominates_all = !latches.empty();
              for (const BasicBlock *latch : latches)
                dominates_all &=
                    dominators.dominates(store, latch->getTerminator());
              mandatory |= dominates_all;
              continue;
            }
          }
          if (isModSet(aa.getModRefInfo(&other, MemoryLocation::get(load))))
            return nullptr;
        }
      return mandatory ? result : nullptr;
    }
    if (const auto *cmp = dyn_cast<ICmpInst>(inst)) {
      auto *a = atBackedge(cmp->getOperand(0), loop, depth - 1);
      auto *b = atBackedge(cmp->getOperand(1), loop, depth - 1);
      return a && b ? dyn_cast<ConstantInt>(
                          ConstantExpr::getICmp(cmp->getPredicate(), a, b))
                    : nullptr;
    }
    if (const auto *op = dyn_cast<BinaryOperator>(inst)) {
      auto *a = atBackedge(op->getOperand(0), loop, depth - 1);
      auto *b = atBackedge(op->getOperand(1), loop, depth - 1);
      return a && b ? dyn_cast<ConstantInt>(
                          ConstantExpr::get(op->getOpcode(), a, b))
                    : nullptr;
    }
    if (const auto *cast = dyn_cast<CastInst>(inst)) {
      auto *input = atBackedge(cast->getOperand(0), loop, depth - 1);
      return input ? dyn_cast<ConstantInt>(ConstantExpr::getCast(
                         cast->getOpcode(), input, cast->getType()))
                   : nullptr;
    }
    return nullptr;
  }

  bool requiresSmallBound(const Value *condition, bool truth, const Loop &loop,
                          unsigned depth) const {
    if (!depth)
      return false;
    if (const auto *op = dyn_cast<BinaryOperator>(condition)) {
      if (op->getOpcode() == Instruction::Xor)
        if (const auto *constant = dyn_cast<ConstantInt>(op->getOperand(1)))
          if (constant->getType()->isIntegerTy(1))
            return requiresSmallBound(op->getOperand(0),
                                      truth != !constant->isZero(), loop,
                                      depth - 1);
    }
    if (const auto *phi = dyn_cast<PHINode>(condition)) {
      const BasicBlock *possible = nullptr;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        const auto *constant = dyn_cast<ConstantInt>(phi->getIncomingValue(i));
        if (constant && constant->isZero() == truth)
          continue;
        if (possible)
          return false;
        possible = phi->getIncomingBlock(i);
      }
      if (!possible || !possible->getSinglePredecessor())
        return false;
      const auto *branch = dyn_cast<BranchInst>(
          possible->getSinglePredecessor()->getTerminator());
      if (!branch || !branch->isConditional())
        return false;
      return requiresSmallBound(branch->getCondition(),
                                branch->getSuccessor(0) == possible, loop,
                                depth - 1);
    }
    const auto *cmp = dyn_cast<ICmpInst>(condition);
    if (!cmp)
      return false;
    auto predicate = truth ? cmp->getPredicate() : cmp->getInversePredicate();
    const auto *bound = dyn_cast<ConstantInt>(cmp->getOperand(1));
    if (!bound ||
        (predicate != ICmpInst::ICMP_SLT && predicate != ICmpInst::ICMP_ULT))
      return false;
    const auto *recurrence = dyn_cast<SCEVAddRecExpr>(
        evolution.getSCEV(const_cast<Value *>(cmp->getOperand(0))));
    if (!recurrence || recurrence->getLoop() != &loop ||
        !recurrence->isAffine())
      return false;
    const auto *start = dyn_cast<SCEVConstant>(recurrence->getStart());
    const auto *step =
        dyn_cast<SCEVConstant>(recurrence->getStepRecurrence(evolution));
    if (!start || !step || step->getAPInt() != 1 ||
        start->getAPInt().isNegative() || bound->isNegative())
      return false;
    APInt distance = bound->getValue().zext(bound->getBitWidth() + 1) -
                     start->getAPInt().zext(bound->getBitWidth() + 1);
    return !distance.isNegative() && distance.ule(16);
  }

  Optional<APInt> upperBound(const Value &value, const Instruction &at,
                             unsigned depth, bool infer_type_bound = false,
                             const BasicBlock *edge_from = nullptr,
                             const BasicBlock *edge_to = nullptr) const {
    if (!depth || !value.getType()->isIntegerTy())
      return None;
    unsigned width = value.getType()->getIntegerBitWidth();
    auto actual = constant_arguments.find(&value);
    if (actual != constant_arguments.end())
      return actual->second.zextOrTrunc(width);
    if (const auto *constant = dyn_cast<ConstantInt>(&value))
      return constant->getValue();
    bool nonnegative =
        isKnownNonNegative(&value, function.getParent()->getDataLayout(), 0,
                           &assumptions, &at, &dominators);
    Optional<APInt> bound;
    std::vector<std::pair<ICmpInst::Predicate, APInt>> guards;
    for (const BasicBlock &block : function) {
      const auto *branch = dyn_cast<BranchInst>(block.getTerminator());
      if (!branch || !branch->isConditional())
        continue;
      bool selected_edge = branch->getParent() == edge_from && edge_to &&
                           (branch->getSuccessor(0) == edge_to ||
                            branch->getSuccessor(1) == edge_to);
      if (!selected_edge && !dominators.dominates(branch, &at))
        continue;
      bool yes = dominators.dominates(
          BasicBlockEdge(branch->getParent(), branch->getSuccessor(0)),
          at.getParent());
      bool no = dominators.dominates(
          BasicBlockEdge(branch->getParent(), branch->getSuccessor(1)),
          at.getParent());
      if (selected_edge) {
        yes = branch->getSuccessor(0) == edge_to;
        no = branch->getSuccessor(1) == edge_to;
      }
      if (yes == no)
        continue;
      const auto *comparison = dyn_cast<ICmpInst>(branch->getCondition());
      if (!comparison)
        continue;
      auto predicate =
          yes ? comparison->getPredicate() : comparison->getInversePredicate();
      const Value *other = nullptr;
      if (equivalent(&value, comparison->getOperand(0), 16))
        other = comparison->getOperand(1);
      else if (equivalent(&value, comparison->getOperand(1), 16)) {
        other = comparison->getOperand(0);
        predicate = ICmpInst::getSwappedPredicate(predicate);
      }
      if (!other || other == &value)
        continue;
      auto limit = upperBound(*other, *comparison, depth - 1);
      if (!limit)
        continue;
      guards.push_back({predicate, *limit});
      bool exact_negative_one =
          (isa<ConstantInt>(other) || constant_arguments.count(other)) &&
          limit->isAllOnes();
      if ((predicate == ICmpInst::ICMP_SGE && !limit->isNegative()) ||
          (predicate == ICmpInst::ICMP_SGT &&
           (!limit->isNegative() || exact_negative_one)))
        nonnegative = true;
    }
    auto restrict = [&](APInt upper) {
      if (!bound || upper.ult(*bound))
        bound = upper;
    };
    for (auto guard : guards) {
      auto predicate = guard.first;
      const APInt &limit = guard.second;
      if (predicate == ICmpInst::ICMP_EQ && !limit.isNegative())
        restrict(limit);
      if ((predicate == ICmpInst::ICMP_ULT ||
           (predicate == ICmpInst::ICMP_SLT && nonnegative)) &&
          !limit.isZero())
        if (predicate == ICmpInst::ICMP_ULT || !limit.isNegative())
          restrict(limit - 1);
      if (predicate == ICmpInst::ICMP_ULE ||
          (predicate == ICmpInst::ICMP_SLE && nonnegative &&
           !limit.isNegative()))
        restrict(limit);
    }
    if (const auto *cast = dyn_cast<CastInst>(&value)) {
      auto input = upperBound(*cast->getOperand(0), at, depth - 1, true);
      if (input &&
          (cast->getOpcode() == Instruction::ZExt ||
           (cast->getOpcode() == Instruction::SExt && !input->isNegative())))
        restrict(input->zextOrTrunc(width));
    }
    if (const auto *phi = dyn_cast<PHINode>(&value)) {
      Optional<APInt> maximum;
      bool complete = true;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        auto incoming =
            upperBound(*phi->getIncomingValue(i),
                       *phi->getIncomingBlock(i)->getTerminator(), depth - 1,
                       false, phi->getIncomingBlock(i), phi->getParent());
        if (!incoming) {
          complete = false;
          break;
        }
        if (!maximum || maximum->ult(*incoming))
          maximum = *incoming;
      }
      if (complete && maximum)
        restrict(*maximum);
    }
    if (const auto *call = dyn_cast<CallBase>(&value))
      if (const Function *callee = ValueFacts::callee(*call))
        if (!callee->isDeclaration() && callee != &function) {
          Impl child(const_cast<Function &>(*callee));
          unsigned count =
              std::min<unsigned>(call->arg_size(), callee->arg_size());
          for (unsigned i = 0; i < count; ++i)
            if (const auto *constant =
                    dyn_cast<ConstantInt>(call->getArgOperand(i)))
              child.constant_arguments.emplace(callee->getArg(i),
                                               constant->getValue());
          Optional<APInt> maximum;
          bool complete = true;
          for (const BasicBlock &block : *callee)
            if (const auto *ret = dyn_cast<ReturnInst>(block.getTerminator())) {
              if (!ret->getReturnValue()) {
                complete = false;
                break;
              }
              auto output =
                  child.upperBound(*ret->getReturnValue(), *ret, depth - 1);
              if (!output) {
                complete = false;
                break;
              }
              if (!maximum || maximum->ult(*output))
                maximum = *output;
            }
          if (complete && maximum)
            restrict(maximum->zextOrTrunc(width));
        }
    if (const auto *op = dyn_cast<BinaryOperator>(&value)) {
      const auto *constant = dyn_cast<ConstantInt>(op->getOperand(1));
      if (op->getOpcode() == Instruction::And) {
        if (!constant)
          constant = dyn_cast<ConstantInt>(op->getOperand(0));
        if (constant)
          restrict(constant->getValue());
      }
      if (constant && !constant->isZero()) {
        if (op->getOpcode() == Instruction::URem)
          restrict(constant->getValue() - 1);
        if (op->getOpcode() == Instruction::UDiv)
          restrict(APInt::getMaxValue(width).udiv(constant->getValue()));
        if (op->getOpcode() == Instruction::LShr &&
            constant->getValue().ult(width))
          restrict(APInt::getMaxValue(width).lshr(constant->getZExtValue()));
      }
      if (op->getOpcode() == Instruction::Add ||
          op->getOpcode() == Instruction::Mul) {
        auto a = upperBound(*op->getOperand(0), at, depth - 1, true),
             b = upperBound(*op->getOperand(1), at, depth - 1, true);
        if (a && b) {
          APInt left = a->zext(width * 2), right = b->zext(width * 2);
          APInt sum =
              op->getOpcode() == Instruction::Add ? left + right : left * right;
          if (sum.getActiveBits() <= width)
            restrict(sum.trunc(width));
        }
      }
    }
    if (infer_type_bound && nonnegative)
      restrict(APInt::getSignedMaxValue(width));
    return bound;
  }
};

FunctionFacts::FunctionFacts(Function &f) : impl_(new Impl(f)) {}
FunctionFacts::~FunctionFacts() = default;
bool FunctionFacts::equivalent(const Value &a, const Value &b) const {
  return impl_->equivalent(&a, &b, 32);
}

const Value &FunctionFacts::pointerOrigin(const Value &value) const {
  return *impl_->pointerOrigin(&value, 16);
}
bool FunctionFacts::reaches(const Instruction &from,
                            const Instruction &to) const {
  if (from.getFunction() != to.getFunction())
    return false;
  if (from.getParent() == to.getParent() && from.comesBefore(&to))
    return true;
  for (const BasicBlock *successor : successors(from.getParent()))
    if (blockReaches(successor, to.getParent()))
      return true;
  return false;
}
bool FunctionFacts::blocksConnected(const BasicBlock &a,
                                    const BasicBlock &b) const {
  return blockReaches(&a, &b) || blockReaches(&b, &a);
}
bool FunctionFacts::recursive(const CallBase &call) const {
  const Function *target = ValueFacts::callee(call);
  if (!target)
    return false;
  SmallVector<const Function *, 16> todo{target};
  SmallPtrSet<const Function *, 32> seen;
  while (!todo.empty()) {
    const Function *function = todo.pop_back_val();
    if (function == call.getFunction())
      return true;
    if (!seen.insert(function).second)
      continue;
    for (const BasicBlock &block : *function)
      for (const Instruction &inst : block)
        if (const auto *next = dyn_cast<CallBase>(&inst))
          if (const Function *callee = ValueFacts::callee(*next))
            todo.push_back(callee);
  }
  return false;
}
bool FunctionFacts::repeatedStackAllocation(
    const AllocaInst &allocation) const {
  for (const Loop *loop = impl_->loops.getLoopFor(allocation.getParent()); loop;
       loop = loop->getParentLoop()) {
    bool restored = false;
    for (const BasicBlock *block : loop->blocks())
      for (const Instruction &inst : *block)
        if (const auto *call = dyn_cast<CallBase>(&inst))
          if (const Function *target = ValueFacts::callee(*call))
            restored |= target->getIntrinsicID() == Intrinsic::stackrestore;
    if (restored)
      continue; // Excludes scoped VLAs; a deliberate subset.
    const SCEV *count = impl_->evolution.getConstantMaxBackedgeTakenCount(loop);
    if (const auto *constant = dyn_cast<SCEVConstant>(count))
      if (constant->getAPInt().ule(16))
        continue;
    SmallVector<BasicBlock *, 8> exiting, latches;
    loop->getExitingBlocks(exiting);
    loop->getLoopLatches(latches);
    bool small = false;
    for (const BasicBlock *block : exiting) {
      bool mandatory = !latches.empty();
      for (const BasicBlock *latch : latches)
        mandatory &= impl_->dominators.dominates(block, latch);
      if (!mandatory)
        continue;
      if (const auto *bound =
              dyn_cast<SCEVConstant>(impl_->evolution.getExitCount(
                  loop, block, ScalarEvolution::ConstantMaximum)))
        if (bound->getAPInt().ule(16)) {
          small = true;
          break;
        }
      if (const auto *branch = dyn_cast<BranchInst>(block->getTerminator()))
        if (branch->isConditional()) {
          bool continues_true = loop->contains(branch->getSuccessor(0));
          if (impl_->requiresSmallBound(branch->getCondition(), continues_true,
                                        *loop, 16)) {
            small = true;
            break;
          }
          if (const ConstantInt *condition =
                  impl_->atBackedge(branch->getCondition(), *loop, 16))
            if (!loop->contains(
                    branch->getSuccessor(condition->isZero() ? 1 : 0))) {
              small = true;
              break;
            }
        }
    }
    if (small)
      continue;
    return true;
  }
  return false;
}
bool FunctionFacts::onlyZeroChecked(const CallBase &call) const {
  bool zero = false, safe = false;
  for (const BasicBlock &block : impl_->function)
    for (const Instruction &inst : block)
      if (const auto *cmp = dyn_cast<ICmpInst>(&inst)) {
        const ConstantInt *constant = nullptr;
        ICmpInst::Predicate predicate = cmp->getPredicate();
        if (equivalent(call, *cmp->getOperand(0)))
          constant = dyn_cast<ConstantInt>(cmp->getOperand(1));
        else if (equivalent(call, *cmp->getOperand(1))) {
          constant = dyn_cast<ConstantInt>(cmp->getOperand(0));
          predicate = ICmpInst::getSwappedPredicate(predicate);
        }
        if (!constant)
          continue;
        bool branch = false;
        for (const User *user : cmp->users())
          if (const auto *br = dyn_cast<BranchInst>(user))
            branch |= br->isConditional();
        if (!branch)
          continue;
        if (ICmpInst::isEquality(predicate)) {
          zero |= constant->isZero();
          safe |= constant->isNegative() ||
                  constant->getValue().isStrictlyPositive();
        } else if (ICmpInst::isSigned(predicate))
          safe = true;
      }
  return zero && !safe;
}

bool FunctionFacts::nonNegative(const Value &value,
                                const Instruction &at) const {
  return isKnownNonNegative(&value,
                            impl_->function.getParent()->getDataLayout(), 0,
                            &impl_->assumptions, &at, &impl_->dominators);
}

bool FunctionFacts::mayOverflowPositively(const BinaryOperator &add) const {
  if (!add.getType()->isIntegerTy())
    return false;
  unsigned width = add.getType()->getIntegerBitWidth();
  ConstantRange left = impl_->evolution.getSignedRange(
      impl_->evolution.getSCEV(add.getOperand(0)));
  ConstantRange right = impl_->evolution.getSignedRange(
      impl_->evolution.getSCEV(add.getOperand(1)));
  APInt sum = left.getSignedMax().sext(width + 1) +
              right.getSignedMax().sext(width + 1);
  return sum.sgt(APInt::getSignedMaxValue(width).sext(width + 1));
}

bool FunctionFacts::boundedAt(const Value &value, const Instruction &at) const {
  if (value.getType()->isIntegerTy(1))
    return true;
  auto bound = impl_->upperBound(value, at, 12);
  return bound && !bound->isAllOnes();
}

Optional<ConstantRange>
FunctionFacts::integerRangeAt(const Value &value, const Instruction &at) const {
  if (!value.getType()->isIntegerTy() ||
      value.getType()->getIntegerBitWidth() > 64)
    return None;
  ConstantRange range = impl_->evolution.getSignedRange(
      impl_->evolution.getSCEV(const_cast<Value *>(&value)));
  for (const BasicBlock &block : impl_->function) {
    const auto *branch = dyn_cast<BranchInst>(block.getTerminator());
    if (!branch || !branch->isConditional())
      continue;
    const auto *cmp = dyn_cast<ICmpInst>(branch->getCondition());
    if (!cmp)
      continue;
    auto predicate = cmp->getPredicate();
    const ConstantInt *constant = nullptr;
    if (equivalent(value, *cmp->getOperand(0)))
      constant = dyn_cast<ConstantInt>(cmp->getOperand(1));
    else if (equivalent(value, *cmp->getOperand(1))) {
      constant = dyn_cast<ConstantInt>(cmp->getOperand(0));
      predicate = ICmpInst::getSwappedPredicate(predicate);
    }
    if (!constant)
      continue;
    bool yes = impl_->dominators.dominates(
        BasicBlockEdge(&block, branch->getSuccessor(0)), at.getParent());
    bool no = impl_->dominators.dominates(
        BasicBlockEdge(&block, branch->getSuccessor(1)), at.getParent());
    if (yes == no)
      continue;
    if (no)
      predicate = ICmpInst::getInversePredicate(predicate);
    range = range.intersectWith(
        ConstantRange::makeExactICmpRegion(predicate, constant->getValue()));
  }
  if (range.isEmptySet())
    return None;
  return range;
}

} // namespace pdg
