#include "IR/PDG/Analyses/ArithmeticAnalysis.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"

#include "IR/PDG/Analyses/TaintAnalysis.h"
#include "IR/PDG/Models/FunctionFacts.h"
#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Models/ValueFacts.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

using namespace llvm;
namespace pdg {
namespace {
// Wide mathematical bounds prevent the analysis itself from overflowing when
// comparing a product against the range of an LLVM integer type (up to i128).
constexpr unsigned MathWidth = 512;
APInt wide(const APInt &v, bool sign) {
  return sign ? v.sextOrTrunc(MathWidth) : v.zextOrTrunc(MathWidth);
}
APInt minimum(const APInt &a, const APInt &b) { return a.slt(b) ? a : b; }
APInt maximum(const APInt &a, const APInt &b) { return a.sgt(b) ? a : b; }
struct Interval {
  APInt low;
  APInt high;
  bool contains(const APInt &v) const { return low.sle(v) && high.sge(v); }
  bool subset(const Interval &other) const {
    return low.sge(other.low) && high.sle(other.high);
  }
};
Interval full(unsigned bits, bool sign) {
  if (bits == 0 || bits >= MathWidth)
    return {APInt::getSignedMinValue(MathWidth),
            APInt::getSignedMaxValue(MathWidth)};
  if (sign)
    return {wide(APInt::getSignedMinValue(bits), true),
            wide(APInt::getSignedMaxValue(bits), true)};
  return {APInt(MathWidth, 0), wide(APInt::getMaxValue(bits), false)};
}
Interval hull(const Interval &a, const Interval &b) {
  return {minimum(a.low, b.low), maximum(a.high, b.high)};
}
bool arithmetic(unsigned opcode) {
  return opcode == Instruction::Add || opcode == Instruction::Sub ||
         opcode == Instruction::Mul || opcode == Instruction::Shl;
}

struct Engine {
  Function &function;
  TargetLibraryInfoImpl tli_impl;
  TargetLibraryInfo tli;
  AssumptionCache assumptions;
  DominatorTree dominators;
  BasicAAResult basic_aa;
  AAResults aliases;
  std::unique_ptr<MemorySSA> memory_;
  FunctionFacts facts;
  std::map<const Value *, bool> declared_sign;
  std::map<const LoadInst *, std::vector<const Value *>> load_definitions;
  using RangeKey = std::tuple<const Value *, bool, const Instruction *>;
  std::map<RangeKey, Interval> ranges;
  std::set<RangeKey> active_ranges;

  MemorySSA &getMemory() {
    if (!memory_)
      memory_ = std::make_unique<MemorySSA>(function, &aliases, &dominators);
    return *memory_;
  }

  explicit Engine(Function &f)
      : function(f), tli_impl(Triple(f.getParent()->getTargetTriple())),
        tli(tli_impl), assumptions(f), dominators(f),
        basic_aa(f.getParent()->getDataLayout(), f, tli, assumptions,
                 &dominators),
        aliases(tli), facts(f) {
    aliases.addAAResult(basic_aa);
    for (const Instruction &inst : instructions(f))
      if (const auto *debug = dyn_cast<DbgVariableIntrinsic>(&inst)) {
        const DIType *type = debug->getVariable()->getType();
        for (unsigned i = 0; i < 16; ++i) {
          if (const auto *derived = dyn_cast_or_null<DIDerivedType>(type))
            type = derived->getBaseType();
          else
            break;
        }
        if (const auto *basic = dyn_cast_or_null<DIBasicType>(type)) {
          unsigned encoding = basic->getEncoding();
          if (encoding == dwarf::DW_ATE_signed ||
              encoding == dwarf::DW_ATE_signed_char ||
              encoding == dwarf::DW_ATE_unsigned ||
              encoding == dwarf::DW_ATE_unsigned_char)
            for (const Value *location : debug->location_ops())
              declared_sign[location] = encoding == dwarf::DW_ATE_signed ||
                                        encoding == dwarf::DW_ATE_signed_char;
        }
      }
  }

  Optional<bool> signedness(const Value &v, unsigned depth = 0) {
    if (depth > 12)
      return None;
    auto declared = declared_sign.find(&v);
    if (declared != declared_sign.end())
      return declared->second;
    if (const auto *load = dyn_cast<LoadInst>(&v)) {
      const Value *object = getUnderlyingObject(load->getPointerOperand());
      auto sign = declared_sign.find(object);
      if (sign != declared_sign.end())
        return sign->second;
    }
    if (isa<SExtInst>(v))
      return true;
    if (isa<ZExtInst>(v))
      return false;
    if (const auto *op = dyn_cast<BinaryOperator>(&v)) {
      if (op->hasNoSignedWrap())
        return true;
      // A sext describes bit extension, not the signedness of a subsequent
      // operation: `(unsigned)(signed char)x` is also lowered as sext. Only
      // byte-sized operations without flags recover signedness from the
      // original local byte variables (Clang's ++/-- lowering).
      if (!op->getType()->isIntegerTy() ||
          op->getType()->getIntegerBitWidth() > 8)
        return None;
      auto a = signedness(*op->getOperand(0), depth + 1);
      auto b = signedness(*op->getOperand(1), depth + 1);
      if (a && b && *a == *b)
        return a;
      if (a && isa<ConstantInt>(op->getOperand(1)))
        return a;
      if (b && isa<ConstantInt>(op->getOperand(0)))
        return b;
    }
    return None;
  }

  // A scalar spill is read from its MemorySSA reaching definition. May-alias
  // writes, volatile/atomic loads and live-on-entry values stay unknown.
  void definitions(const LoadInst &load, MemoryAccess *access,
                   std::vector<const Value *> &out,
                   SmallPtrSetImpl<const MemoryAccess *> &seen) {
    if (!seen.insert(access).second) {
      return;
    }
    if (auto *phi = dyn_cast<MemoryPhi>(access)) {
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
        definitions(load, phi->getIncomingValue(i), out, seen);
    } else if (auto *def = dyn_cast<MemoryDef>(access)) {
      if (const auto *call = dyn_cast_or_null<CallBase>(def->getMemoryInst()))
        if (isa<AllocaInst>(getUnderlyingObject(load.getPointerOperand())) &&
            !PointerMayBeCaptured(getUnderlyingObject(load.getPointerOperand()),
                                  true, true)) {
          bool inaccessible = true;
          for (const Value *argument : call->args())
            if (argument->getType()->isPointerTy() &&
                aliases.alias(MemoryLocation::getBeforeOrAfter(argument),
                              MemoryLocation::get(&load)) !=
                    AliasResult::NoAlias)
              inaccessible = false;
          if (inaccessible) {
            definitions(load, def->getDefiningAccess(), out, seen);
            return;
          }
        }
      if (def->getMemoryInst() &&
          !isModSet(aliases.getModRefInfo(def->getMemoryInst(),
                                          MemoryLocation::get(&load)))) {
        definitions(load, def->getDefiningAccess(), out, seen);
        return;
      }
      if (const auto *copy =
              dyn_cast_or_null<MemTransferInst>(def->getMemoryInst())) {
        const DataLayout &layout = function.getParent()->getDataLayout();
        int64_t load_offset = 0, destination_offset = 0;
        const Value *load_base = GetPointerBaseWithConstantOffset(
            load.getPointerOperand(), load_offset, layout);
        const Value *destination_base = GetPointerBaseWithConstantOffset(
            copy->getDest(), destination_offset, layout);
        const auto *count = dyn_cast<ConstantInt>(copy->getLength());
        const auto *source = dyn_cast<Constant>(copy->getSource());
        const auto *global =
            source ? dyn_cast<GlobalVariable>(getUnderlyingObject(source))
                   : nullptr;
        uint64_t bytes = layout.getTypeStoreSize(load.getType());
        if (!copy->isVolatile() && load_base == destination_base && count &&
            source && global && global->isConstant() &&
            load_offset >= destination_offset) {
          uint64_t offset = load_offset - destination_offset;
          uint64_t length = count->getValue().getLimitedValue();
          if (offset <= length && bytes <= length - offset)
            if (Constant *folded = ConstantFoldLoadFromConstPtr(
                    const_cast<Constant *>(source), load.getType(),
                    APInt(layout.getPointerSizeInBits(), offset), layout)) {
              out.push_back(folded);
              return;
            }
        }
        out.push_back(&load);
      } else if (const auto *store =
                     dyn_cast_or_null<StoreInst>(def->getMemoryInst())) {
        if (!store->isVolatile() && !store->isAtomic() &&
            store->getValueOperand()->getType() == load.getType() &&
            aliases.alias(MemoryLocation::get(&load),
                          MemoryLocation::get(store)) == AliasResult::MustAlias)
          out.push_back(store->getValueOperand());
        else
          out.push_back(&load);
      } else {
        out.push_back(&load);
      }
    } else {
      out.push_back(&load);
    }
  }
  std::vector<const Value *> definitions(const LoadInst &load) {
    auto found = load_definitions.find(&load);
    if (found != load_definitions.end())
      return found->second;
    if (load.isVolatile() || load.isAtomic())
      return {&load};
    if (const auto *pointer = dyn_cast<Constant>(load.getPointerOperand()))
      if (const auto *global =
              dyn_cast<GlobalVariable>(getUnderlyingObject(pointer)))
        if (global->isConstant())
          if (Constant *folded = ConstantFoldLoadFromConstPtr(
                  const_cast<Constant *>(pointer), load.getType(),
                  function.getParent()->getDataLayout()))
            return {folded};
    auto &mssa = getMemory();
    auto *access = mssa.getMemoryAccess(&load);
    if (!access)
      return {&load};
    std::vector<const Value *> result;
    SmallPtrSet<const MemoryAccess *, 16> seen;
    definitions(load, mssa.getWalker()->getClobberingMemoryAccess(access),
                result, seen);
    if (result.empty())
      result.push_back(&load);
    load_definitions.emplace(&load, result);
    return result;
  }

  // Only edges which dominate the use contribute facts. Successor-block
  // dominance alone would incorrectly refine facts at a post-if join.
  template <class Callback>
  void guards(const Instruction &at, Callback callback) {
    const auto *domNode =
        dominators.getNode(const_cast<BasicBlock *>(at.getParent()));
    if (!domNode)
      return;
    for (const auto *cur = domNode->getIDom(); cur; cur = cur->getIDom()) {
      const BasicBlock &block = *cur->getBlock();
      const auto *branch = dyn_cast<BranchInst>(block.getTerminator());
      if (!branch || !branch->isConditional())
        continue;
      const auto *cmp = dyn_cast<ICmpInst>(branch->getCondition());
      if (!cmp)
        continue;
      bool yes = dominators.dominates(
          BasicBlockEdge(&block, branch->getSuccessor(0)), at.getParent());
      bool no = dominators.dominates(
          BasicBlockEdge(&block, branch->getSuccessor(1)), at.getParent());
      if (yes != no)
        callback(*cmp, yes ? cmp->getPredicate() : cmp->getInversePredicate());
    }
  }

  Interval refine(Interval range, const Value &v, bool sign,
                  const Instruction &at) {
    guards(at, [&](const ICmpInst &cmp, CmpInst::Predicate pred) {
      const Value *left = cmp.getOperand(0), *right = cmp.getOperand(1);
      if (!facts.equivalent(v, *left)) {
        if (!facts.equivalent(v, *right))
          return;
        std::swap(left, right);
        pred = CmpInst::getSwappedPredicate(pred);
      }
      const auto *constant = dyn_cast<ConstantInt>(right);
      if (!constant || (CmpInst::isSigned(pred) && !sign) ||
          (CmpInst::isUnsigned(pred) && sign))
        return;
      APInt c = wide(constant->getValue(), sign);
      APInt one(MathWidth, 1);
      switch (pred) {
      case CmpInst::ICMP_EQ:
        range.low = maximum(range.low, c);
        range.high = minimum(range.high, c);
        break;
      case CmpInst::ICMP_SLT:
      case CmpInst::ICMP_ULT:
        range.high = minimum(range.high, c - one);
        break;
      case CmpInst::ICMP_SLE:
      case CmpInst::ICMP_ULE:
        range.high = minimum(range.high, c);
        break;
      case CmpInst::ICMP_SGT:
      case CmpInst::ICMP_UGT:
        range.low = maximum(range.low, c + one);
        break;
      case CmpInst::ICMP_SGE:
      case CmpInst::ICMP_UGE:
        range.low = maximum(range.low, c);
        break;
      default:
        break;
      }
    });
    return range;
  }

  Interval range(const Value &v, bool sign, const Instruction &at,
                 unsigned depth = 0) {
    if (!v.getType()->isIntegerTy())
      return {APInt::getSignedMinValue(MathWidth),
              APInt::getSignedMaxValue(MathWidth)};
    unsigned bits = v.getType()->getIntegerBitWidth();
    Interval result = full(bits, sign);
    if (bits == 0 || bits > 128 || depth > 24)
      return result;
    RangeKey key{&v, sign, &at};
    auto cached = ranges.find(key);
    if (cached != ranges.end())
      return cached->second;
    if (!active_ranges.insert(key).second)
      return result;
    if (const auto *c = dyn_cast<ConstantInt>(&v)) {
      APInt x = wide(c->getValue(), sign);
      result = {x, x};
    } else if (const auto *load = dyn_cast<LoadInst>(&v)) {
      bool first = true;
      for (const Value *defined : definitions(*load)) {
        Interval next = defined == load ? full(bits, sign)
                                        : range(*defined, sign, at, depth + 1);
        result = first ? next : hull(result, next);
        first = false;
      }
    } else if (const auto *cast = dyn_cast<CastInst>(&v)) {
      if (cast->getOpcode() == Instruction::ZExt)
        result = range(*cast->getOperand(0), false, at, depth + 1);
      else if (cast->getOpcode() == Instruction::SExt) {
        result = range(*cast->getOperand(0), true, at, depth + 1);
        if (!sign && result.low.isNegative()) {
          if (result.high.isNegative()) {
            APInt modulus = APInt(MathWidth, 1).shl(bits);
            result.low += modulus;
            result.high += modulus;
          } else {
            // The unsigned image crosses the modular boundary and has two
            // components, so a single interval must conservatively be full.
            result = full(bits, false);
          }
        }
      } else if (cast->getOpcode() == Instruction::Trunc) {
        Interval next = range(*cast->getOperand(0), sign, at, depth + 1);
        if (next.subset(result))
          result = next;
      }
    } else if (const auto *phi = dyn_cast<PHINode>(&v)) {
      bool first = true;
      for (const Value *incoming : phi->incoming_values()) {
        Interval next = range(*incoming, sign, at, depth + 1);
        result = first ? next : hull(result, next);
        first = false;
      }
    } else if (const auto *select = dyn_cast<SelectInst>(&v)) {
      result = hull(range(*select->getTrueValue(), sign, at, depth + 1),
                    range(*select->getFalseValue(), sign, at, depth + 1));
    } else if (const auto *op = dyn_cast<BinaryOperator>(&v)) {
      if (arithmetic(op->getOpcode())) {
        Interval calculated = mathematical(*op, sign, at, depth + 1);
        if (calculated.subset(result))
          result = calculated;
      } else {
        ConstantRange known = computeConstantRange(
            &v, sign, false, &assumptions, &at, &dominators);
        result = sign ? Interval{wide(known.getSignedMin(), true),
                                 wide(known.getSignedMax(), true)}
                      : Interval{wide(known.getUnsignedMin(), false),
                                 wide(known.getUnsignedMax(), false)};
      }
    }
    result = refine(result, v, sign, at);
    active_ranges.erase(key);
    ranges.emplace(key, result);
    return result;
  }

  Interval mathematical(const BinaryOperator &op, bool sign,
                        const Instruction &at, unsigned depth = 0) {
    Interval a = range(*op.getOperand(0), sign, at, depth + 1);
    Interval b = range(*op.getOperand(1), sign, at, depth + 1);
    switch (op.getOpcode()) {
    case Instruction::Add:
      return {a.low + b.low, a.high + b.high};
    case Instruction::Sub:
      return {a.low - b.high, a.high - b.low};
    case Instruction::Mul: {
      APInt x = a.low * b.low, y = a.low * b.high, z = a.high * b.low,
            w = a.high * b.high;
      return {minimum(minimum(x, y), minimum(z, w)),
              maximum(maximum(x, y), maximum(z, w))};
    }
    case Instruction::Shl:
      if (b.low == b.high && b.low.isNonNegative() && b.low.ult(128)) {
        unsigned shift = b.low.getLimitedValue();
        return {a.low.shl(shift), a.high.shl(shift)};
      }
      return {APInt::getSignedMinValue(MathWidth),
              APInt::getSignedMaxValue(MathWidth)};
    default:
      return op.getType()->isIntegerTy()
                 ? full(op.getType()->getIntegerBitWidth(), sign)
                 : Interval{APInt::getSignedMinValue(MathWidth),
                            APInt::getSignedMaxValue(MathWidth)};
    }
  }
  bool mayOverflow(const BinaryOperator &op, bool sign) {
    if (!op.getType()->isIntegerTy() || op.getType()->getIntegerBitWidth() > 128)
      return false;
    return !mathematical(op, sign, op)
                .subset(full(op.getType()->getIntegerBitWidth(), sign));
  }

  bool excludes(const Value &v, const APInt &value, const Instruction &at) {
    if (isKnownNonZero(&v, function.getParent()->getDataLayout(), 0,
                       &assumptions, &at, &dominators) &&
        value.isZero())
      return true;
    if (v.getType()->isIntegerTy() && v.getType()->getIntegerBitWidth() <= 128 &&
        (!range(v, true, at).contains(wide(value, true)) ||
         !range(v, false, at).contains(wide(value, false))))
      return true;
    bool excluded = false;
    guards(at, [&](const ICmpInst &cmp, CmpInst::Predicate pred) {
      const Value *lhs = cmp.getOperand(0), *rhs = cmp.getOperand(1);
      if (!facts.equivalent(v, *lhs)) {
        if (!facts.equivalent(v, *rhs))
          return;
        std::swap(lhs, rhs);
        pred = CmpInst::getSwappedPredicate(pred);
      }
      if (const auto *c = dyn_cast<ConstantInt>(rhs))
        excluded |= !ICmpInst::compare(value, c->getValue(), pred);
    });
    return excluded;
  }

  bool notLess(const Value &left, const Value &right, const Instruction &at) {
    if (facts.equivalent(left, right))
      return true;
    if (const auto *op = dyn_cast<BinaryOperator>(&right))
      if ((op->getOpcode() == Instruction::And ||
           op->getOpcode() == Instruction::LShr ||
           op->getOpcode() == Instruction::UDiv) &&
          facts.equivalent(left, *op->getOperand(0))) {
        if (op->getOpcode() != Instruction::UDiv)
          return true;
        if (const auto *c = dyn_cast<ConstantInt>(op->getOperand(1)))
          if (!c->isZero())
            return true;
      }
    bool result = false;
    guards(at, [&](const ICmpInst &cmp, CmpInst::Predicate pred) {
      if (facts.equivalent(left, *cmp.getOperand(1)) &&
          facts.equivalent(right, *cmp.getOperand(0)))
        pred = CmpInst::getSwappedPredicate(pred);
      else if (!facts.equivalent(left, *cmp.getOperand(0)) ||
               !facts.equivalent(right, *cmp.getOperand(1)))
        return;
      result |= pred == CmpInst::ICMP_UGE || pred == CmpInst::ICMP_UGT ||
                pred == CmpInst::ICMP_EQ;
    });
    return result;
  }

  void leaves(const Value &v, std::vector<const Value *> &out,
              unsigned depth = 0, bool multiply_only = false) {
    if (multiply_only) {
      // Preserve distinct factors of a multiplication tree (e.g., x * x has
      // two nonconstant factors) while preventing exponential re-traversal of
      // shared/cyclic sub-expressions inside each factor.
      SmallVector<std::pair<const Value *, unsigned>, 16> factors;
      SmallPtrSet<const Value *, 16> seen_mul;
      SmallVector<std::pair<const Value *, unsigned>, 16> work;
      work.push_back({&v, depth});
      while (!work.empty()) {
        auto [cur, d] = work.pop_back_val();
        if (d > 24) {
          factors.push_back({cur, d});
          continue;
        }
        if (const auto *op = dyn_cast<BinaryOperator>(cur)) {
          if (op->getOpcode() == Instruction::Mul && seen_mul.insert(op).second) {
            work.push_back({op->getOperand(1), d + 1});
            work.push_back({op->getOperand(0), d + 1});
            continue;
          }
        }
        factors.push_back({cur, d});
      }
      for (const auto &factor : factors) {
        std::deque<std::pair<const Value *, unsigned>> queue;
        SmallPtrSet<const Value *, 32> visited;
        queue.push_back(factor);
        visited.insert(factor.first);
        while (!queue.empty()) {
          const Value *cur = queue.front().first;
          unsigned d = queue.front().second;
          queue.pop_front();
          if (d > 24) {
            out.push_back(cur);
            continue;
          }
          auto enqueue = [&](const Value *next) {
            if (visited.insert(next).second)
              queue.push_back({next, d + 1});
          };
          if (const auto *load = dyn_cast<LoadInst>(cur)) {
            for (const Value *def : definitions(*load)) {
              if (def == load)
                out.push_back(def);
              else
                enqueue(def);
            }
          } else if (const auto *cast = dyn_cast<CastInst>(cur)) {
            if (cast->getOperand(0)->getType()->isIntegerTy())
              enqueue(cast->getOperand(0));
          } else if (const auto *phi = dyn_cast<PHINode>(cur)) {
            for (const Value *incoming : phi->incoming_values())
              enqueue(incoming);
          } else if (const auto *select = dyn_cast<SelectInst>(cur)) {
            enqueue(select->getTrueValue());
            enqueue(select->getFalseValue());
          } else if (const auto *op = dyn_cast<BinaryOperator>(cur)) {
            if (op->getOpcode() == Instruction::Mul) {
              for (const Value *operand : op->operands())
                enqueue(operand);
            } else {
              out.push_back(cur);
            }
          } else {
            out.push_back(cur);
          }
        }
      }
      return;
    }

    // BFS visits each node at its minimum depth first, producing the exact
    // same set of leaves within the depth-24 bound in O(|V| + |E|) time.
    std::deque<std::pair<const Value *, unsigned>> queue;
    SmallPtrSet<const Value *, 32> visited;
    SmallPtrSet<const Value *, 32> emitted;
    queue.push_back({&v, depth});
    visited.insert(&v);
    auto emitLeaf = [&](const Value *leaf) {
      if (emitted.insert(leaf).second)
        out.push_back(leaf);
    };
    while (!queue.empty()) {
      const Value *cur = queue.front().first;
      unsigned d = queue.front().second;
      queue.pop_front();
      if (d > 24) {
        emitLeaf(cur);
        continue;
      }
      auto enqueue = [&](const Value *next) {
        if (visited.insert(next).second)
          queue.push_back({next, d + 1});
      };
      if (const auto *load = dyn_cast<LoadInst>(cur)) {
        for (const Value *def : definitions(*load)) {
          if (def == load)
            emitLeaf(def);
          else
            enqueue(def);
        }
      } else if (const auto *cast = dyn_cast<CastInst>(cur)) {
        if (cast->getOperand(0)->getType()->isIntegerTy())
          enqueue(cast->getOperand(0));
      } else if (const auto *phi = dyn_cast<PHINode>(cur)) {
        for (const Value *incoming : phi->incoming_values())
          enqueue(incoming);
      } else if (const auto *select = dyn_cast<SelectInst>(cur)) {
        enqueue(select->getTrueValue());
        enqueue(select->getFalseValue());
      } else if (const auto *op = dyn_cast<BinaryOperator>(cur)) {
        for (const Value *operand : op->operands())
          enqueue(operand);
      } else {
        emitLeaf(cur);
      }
    }
  }
};

bool zeroReturningLibrary(const Function &function) {
  static const std::set<std::string> names = {"iswalpha",
                                              "iswlower",
                                              "iswprint",
                                              "iswspace",
                                              "iswblank",
                                              "iswupper",
                                              "iswcntrl",
                                              "iswctype",
                                              "iswalnum",
                                              "iswgraph",
                                              "iswxdigit",
                                              "iswdigit",
                                              "iswpunct",
                                              "isblank",
                                              "isupper",
                                              "isgraph",
                                              "isalnum",
                                              "ispunct",
                                              "islower",
                                              "isspace",
                                              "isprint",
                                              "isxdigit",
                                              "iscntrl",
                                              "isdigit",
                                              "isalpha",
                                              "timespec_get",
                                              "feof",
                                              "atomic_is_lock_free",
                                              "atomic_compare_exchange",
                                              "thrd_equal",
                                              "isfinite",
                                              "islessequal",
                                              "isnan",
                                              "isgreater",
                                              "signbit",
                                              "isinf",
                                              "islessgreater",
                                              "isnormal",
                                              "isless",
                                              "isgreaterequal",
                                              "isunordered",
                                              "ferror",
                                              "thrd_sleep",
                                              "feenv",
                                              "feholdexcept",
                                              "feclearexcept",
                                              "feexceptflag",
                                              "feupdateenv",
                                              "remove",
                                              "fflush",
                                              "setvbuf",
                                              "fgetpos",
                                              "fsetpos",
                                              "fclose",
                                              "rename",
                                              "fseek",
                                              "raise",
                                              "getc",
                                              "atoi"};
  return names.count(ValueFacts::functionBaseName(function));
}

struct Queries {
  ArithmeticAnalysisResult result;
  std::map<const Function *, std::unique_ptr<Engine>> engines;
  std::map<const Function *, std::set<unsigned>> allocation_parameters;
  std::map<const Function *, SmallVector<const BinaryOperator *, 4>>
      div_instructions;
  const TaintFlowResult *taint;
  std::set<std::pair<std::string, const Instruction *>> reported;

  explicit Queries(const TaintFlowResult *flow) : taint(flow) {}
  Engine &engine(const Function &f) {
    auto &entry = engines[&f];
    if (!entry)
      entry = std::make_unique<Engine>(const_cast<Function &>(f));
    return *entry;
  }
  const SmallVector<const BinaryOperator *, 4> &
  divisionsIn(const Function &f) {
    auto it = div_instructions.find(&f);
    if (it != div_instructions.end())
      return it->second;
    auto &vec = div_instructions[&f];
    for (const Instruction &inst : instructions(f)) {
      const auto *div = dyn_cast<BinaryOperator>(&inst);
      if (div && div->getType()->isIntegerTy() &&
          (div->getOpcode() == Instruction::SDiv ||
           div->getOpcode() == Instruction::UDiv ||
           div->getOpcode() == Instruction::SRem ||
           div->getOpcode() == Instruction::URem))
        vec.push_back(div);
    }
    return vec;
  }
  void report(StringRef id, const Instruction &site, StringRef message,
              std::vector<const Instruction *> evidence = {}) {
    if (reported.emplace(id.str(), &site).second)
      result.findings.push_back({id.str(), &site, message.str(), evidence});
  }

  // Enumerate demonstrated return alternatives rather than interpreting an
  // unknown declaration as a zero-returning function. Arithmetic preserves
  // machine-width behavior and only bounded finite sets are expanded.
  using ValueMemoKey = std::pair<const Value *, const CallBase *>;
  std::vector<APInt>
  valuesImpl(const Value &v, unsigned depth, const CallBase *context,
             std::set<ValueMemoKey> &active,
             std::map<ValueMemoKey, std::vector<APInt>> &memo,
             bool &hit_cutoff) {
    if (!v.getType()->isIntegerTy())
      return {};
    if (depth > 16) {
      hit_cutoff = true;
      return {};
    }
    if (const auto *c = dyn_cast<ConstantInt>(&v))
      return {c->getValue()};
    ValueMemoKey key{&v, context};
    auto cached = memo.find(key);
    if (cached != memo.end())
      return cached->second;
    if (!active.insert(key).second) {
      hit_cutoff = true;
      return {};
    }
    bool local_cutoff = false;
    auto finish = [&](std::vector<APInt> res) {
      active.erase(key);
      if (local_cutoff)
        hit_cutoff = true;
      else
        memo.emplace(key, res);
      return res;
    };
    if (const auto *argument = dyn_cast<Argument>(&v)) {
      if (context && ValueFacts::callee(*context) == argument->getParent() &&
          argument->getArgNo() < context->arg_size()) {
        const Value *actual = context->getArgOperand(argument->getArgNo());
        auto alternatives =
            valuesImpl(*actual, depth + 1, nullptr, active, memo, local_cutoff);
        Engine &caller = engine(*context->getFunction());
        alternatives.erase(
            std::remove_if(alternatives.begin(), alternatives.end(),
                           [&](const APInt &value) {
                             return caller.excludes(*actual, value, *context);
                           }),
            alternatives.end());
        return finish(std::move(alternatives));
      }
      return finish({});
    }
    std::vector<APInt> result;
    auto append = [&](const Value &next) {
      for (APInt x :
           valuesImpl(next, depth + 1, context, active, memo, local_cutoff))
        if (result.size() < 32 &&
            std::find(result.begin(), result.end(), x) == result.end())
          result.push_back(x);
    };
    if (const auto *load = dyn_cast<LoadInst>(&v)) {
      for (const Value *def : engine(*load->getFunction()).definitions(*load))
        if (def != load)
          append(*def);
    } else if (const auto *phi = dyn_cast<PHINode>(&v)) {
      for (const Value *incoming : phi->incoming_values())
        append(*incoming);
    } else if (const auto *select = dyn_cast<SelectInst>(&v)) {
      append(*select->getTrueValue());
      append(*select->getFalseValue());
    } else if (const auto *cast = dyn_cast<CastInst>(&v)) {
      for (APInt x : valuesImpl(*cast->getOperand(0), depth + 1, context,
                                active, memo, local_cutoff))
        result.push_back(
            cast->getOpcode() == Instruction::SExt
                ? x.sextOrTrunc(v.getType()->getIntegerBitWidth())
                : x.zextOrTrunc(v.getType()->getIntegerBitWidth()));
    } else if (const auto *call = dyn_cast<CallBase>(&v)) {
      if (const Function *target = ValueFacts::callee(*call)) {
        if (zeroReturningLibrary(*target))
          result.emplace_back(v.getType()->getIntegerBitWidth(), 0);
        if (!target->isDeclaration())
          for (const BasicBlock &block : *target)
            if (const auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
              if (ret->getReturnValue())
                for (APInt x : valuesImpl(*ret->getReturnValue(), depth + 1,
                                          call, active, memo, local_cutoff))
                  if (result.size() < 32)
                    result.push_back(x);
      }
    } else if (const auto *op = dyn_cast<BinaryOperator>(&v)) {
      auto lhs = valuesImpl(*op->getOperand(0), depth + 1, context, active,
                            memo, local_cutoff);
      if (!lhs.empty()) {
        auto rhs = valuesImpl(*op->getOperand(1), depth + 1, context, active,
                              memo, local_cutoff);
        for (const APInt &a : lhs)
          for (const APInt &b : rhs) {
            if (result.size() >= 32)
              break;
            if (op->getOpcode() == Instruction::Add)
              result.push_back(a + b);
            else if (op->getOpcode() == Instruction::Sub)
              result.push_back(a - b);
            else if (op->getOpcode() == Instruction::Mul)
              result.push_back(a * b);
          }
      }
    }
    return finish(std::move(result));
  }

  std::vector<APInt> values(const Value &v, unsigned depth = 0,
                            const CallBase *context = nullptr) {
    std::set<ValueMemoKey> active;
    std::map<ValueMemoKey, std::vector<APInt>> memo;
    bool hit_cutoff = false;
    return valuesImpl(v, depth, context, active, memo, hit_cutoff);
  }

  void comparison(const ICmpInst &cmp, Engine &facts) {
    if (cmp.isEquality() || !cmp.getOperand(0)->getType()->isIntegerTy())
      return;
    for (unsigned side = 0; side < 2; ++side) {
      const auto *op = dyn_cast<BinaryOperator>(cmp.getOperand(side));
      const Value *other = cmp.getOperand(1 - side);
      if (!op)
        continue;
      bool sign = cmp.isSigned();
      if (op->getOpcode() == Instruction::Add &&
          (facts.facts.equivalent(*op->getOperand(0), *other) ||
           facts.facts.equivalent(*op->getOperand(1), *other))) {
        // QL's check is type based: both operands have strictly narrower
        // original ranges than the promoted result, regardless of CFG guards.
        const auto *a = dyn_cast<CastInst>(op->getOperand(0));
        const auto *b = dyn_cast<CastInst>(op->getOperand(1));
        if (a && b && (isa<ZExtInst>(a) || isa<SExtInst>(a)) &&
            (isa<ZExtInst>(b) || isa<SExtInst>(b))) {
          Interval ar =
              full(a->getSrcTy()->getIntegerBitWidth(), isa<SExtInst>(a));
          Interval br =
              full(b->getSrcTy()->getIntegerBitWidth(), isa<SExtInst>(b));
          Interval sum{ar.low + br.low, ar.high + br.high};
          if (sum.subset(full(op->getType()->getIntegerBitWidth(), sign)))
            report("cpp/bad-addition-overflow-check", cmp,
                   "Promotion prevents this addition from wrapping at the "
                   "operand width.",
                   {op});
        }
      }
      if (op->getOpcode() == Instruction::Sub && cmp.isUnsigned()) {
        const auto *zero = dyn_cast<ConstantInt>(other);
        CmpInst::Predicate pred =
            side == 0 ? cmp.getPredicate() : cmp.getSwappedPredicate();
        if (zero && zero->isZero() &&
            (pred == CmpInst::ICMP_UGT || pred == CmpInst::ICMP_UGE) &&
            !facts.notLess(*op->getOperand(0), *op->getOperand(1), *op)) {
          Interval left = facts.range(*op->getOperand(0), false, *op);
          Interval right = facts.range(*op->getOperand(1), false, *op);
          if (left.low.slt(right.high))
            report("cpp/unsigned-difference-expression-compared-zero", cmp,
                   "An underflowing unsigned difference remains positive "
                   "instead of stopping this check.",
                   {op});
        }
      }
    }
  }

  void assignment(const StoreInst &store, Engine &facts) {
    const auto *sub = dyn_cast<BinaryOperator>(store.getValueOperand());
    if (!sub || sub->getOpcode() != Instruction::Sub ||
        !sub->getType()->isIntegerTy())
      return;
    facts.guards(store, [&](const ICmpInst &cmp, CmpInst::Predicate pred) {
      if (CmpInst::isEquality(pred))
        return;
      for (unsigned side = 0; side < 2; ++side) {
        const auto *add = dyn_cast<BinaryOperator>(cmp.getOperand(side));
        if (!add || add->getOpcode() != Instruction::Add ||
            !facts.mayOverflow(*add, cmp.isSigned()) ||
            !facts.facts.equivalent(*cmp.getOperand(1 - side),
                                    *sub->getOperand(0)))
          continue;
        for (unsigned operand = 0; operand < 2; ++operand) {
          const auto *load = dyn_cast<LoadInst>(add->getOperand(operand));
          if (load &&
              facts.facts.equivalent(*load->getPointerOperand(),
                                     *store.getPointerOperand()) &&
              facts.facts.equivalent(*add->getOperand(1 - operand),
                                     *sub->getOperand(1)))
            report("cpp/if-statement-addition-overflow", cmp,
                   "The addition in the guard can wrap before the corrective "
                   "subtraction and assignment.",
                   {add, sub, &store});
        }
      }
    });
  }

  void allocation(const CallBase &call, Engine &facts) {
    const Function *target = ValueFacts::callee(call);
    if (!target)
      return;
    std::vector<const Value *> todo;
    SmallPtrSet<const Function *, 16> resolving;
    for (unsigned index : allocationSizeArguments(*target, resolving))
      if (index < call.arg_size())
        todo.push_back(call.getArgOperand(index));
    SmallPtrSet<const Value *, 32> seen;
    while (!todo.empty()) {
      const Value *v = todo.back();
      todo.pop_back();
      if (!seen.insert(v).second)
        continue;
      if (const auto *op = dyn_cast<BinaryOperator>(v)) {
        if (op->getOpcode() == Instruction::Mul &&
            !ValueFacts::integer(*op->getOperand(0)) &&
            !ValueFacts::integer(*op->getOperand(1)) &&
            facts.mayOverflow(*op, op->hasNoSignedWrap()))
          report(
              "cpp/multiplication-overflow-in-alloc", call,
              "A possibly overflowing product flows into the allocation size.",
              {op});
      }
      if (const auto *load = dyn_cast<LoadInst>(v)) {
        for (const Value *def : facts.definitions(*load))
          if (def != load)
            todo.push_back(def);
      } else if (const auto *phi = dyn_cast<PHINode>(v)) {
        for (const Value *incoming : phi->incoming_values())
          todo.push_back(incoming);
      } else if (const auto *inst = dyn_cast<Instruction>(v)) {
        if (isa<CastInst>(inst) || isa<BinaryOperator>(inst) ||
            isa<SelectInst>(inst))
          for (const Value *operand : inst->operands())
            if (operand->getType()->isIntegerTy())
              todo.push_back(operand);
      }
    }
  }

  // Parameter roles are inferred from real allocation calls in definitions.
  // This supports numeric wrappers without names such as "alloc"/"malloc"
  // being treated as evidence of an unknown declaration's behavior.
  std::set<unsigned>
  allocationSizeArguments(const Function &target,
                          SmallPtrSetImpl<const Function *> &resolving) {
    auto known = allocation_parameters.find(&target);
    if (known != allocation_parameters.end())
      return known->second;
    if (!resolving.insert(&target).second || resolving.size() > 12)
      return {};
    std::set<unsigned> result;
    if (LibraryModels::allocation(target) != AllocationKind::Unknown) {
      std::string name = ValueFacts::functionBaseName(target);
      if (name == "calloc")
        result = {0, 1};
      else if (name != "strdup" && name != "strndup")
        result.insert(name == "realloc" || name == "aligned_alloc" ? 1 : 0);
    } else if (!target.isDeclaration()) {
      Engine &target_facts = engine(target);
      for (const Instruction &inst : instructions(target))
        if (const auto *nested = dyn_cast<CallBase>(&inst))
          if (const Function *callee = ValueFacts::callee(*nested))
            for (unsigned index : allocationSizeArguments(*callee, resolving))
              if (index < nested->arg_size()) {
                std::vector<const Value *> leaves;
                target_facts.leaves(*nested->getArgOperand(index), leaves);
                for (const Value *leaf : leaves)
                  if (const auto *argument = dyn_cast<Argument>(leaf))
                    if (argument->getParent() == &target)
                      result.insert(argument->getArgNo());
              }
    }
    resolving.erase(&target);
    allocation_parameters.emplace(&target, result);
    return result;
  }

  void widening(const CastInst &cast, Engine &facts) {
    if ((!isa<SExtInst>(cast) && !isa<ZExtInst>(cast)) ||
        !cast.getType()->isIntegerTy() ||
        !cast.getOperand(0)->getType()->isIntegerTy())
      return;
    const auto *mul = dyn_cast<BinaryOperator>(cast.getOperand(0));
    if (!mul || mul->getOpcode() != Instruction::Mul ||
        !mul->getType()->isIntegerTy() ||
        !facts.mayOverflow(*mul, isa<SExtInst>(cast)))
      return;
    std::vector<const Value *> operands;
    facts.leaves(*mul, operands, 0, true);
    unsigned unknown = 0;
    for (const Value *value : operands)
      if (!isa<ConstantInt>(value) && value->getType()->isIntegerTy() &&
          value->getType()->getIntegerBitWidth() > 8)
        ++unknown;
    if (unknown >= 2)
      report("cpp/integer-multiplication-cast-to-long", *mul,
             "The product may overflow before conversion to the wider integer "
             "type.",
             {&cast});
  }

  void division(const BinaryOperator &op, Engine &facts) {
    if (!op.getType()->isIntegerTy())
      return;
    unsigned opcode = op.getOpcode();
    if (opcode != Instruction::SDiv && opcode != Instruction::UDiv &&
        opcode != Instruction::SRem && opcode != Instruction::URem)
      return;
    const Value &divisor = *op.getOperand(1);
    APInt zero(divisor.getType()->getIntegerBitWidth(), 0);
    if (facts.excludes(divisor, zero, op))
      return;
    auto alternatives = values(divisor);
    if (std::find(alternatives.begin(), alternatives.end(), zero) ==
        alternatives.end())
      return;
    std::vector<const Value *> leaves;
    facts.leaves(divisor, leaves);
    std::vector<const Instruction *> evidence;
    for (const Value *leaf : leaves)
      if (const auto *call = dyn_cast<CallBase>(leaf))
        evidence.push_back(call);
    if (!evidence.empty())
      report("cpp/divide-by-zero-using-return-value", op,
             "A demonstrated function return alternative makes the divisor "
             "zero without an excluding guard.",
             evidence);
  }

  void forwardedDivision(const CallBase &call) {
    const Function *target = ValueFacts::callee(call);
    if (!target || target->isDeclaration())
      return;
    const auto &divs = divisionsIn(*target);
    if (divs.empty())
      return;
    Engine &callee = engine(*target);
    for (const BinaryOperator *div : divs) {
      const Value &divisor = *div->getOperand(1);
      APInt zero(divisor.getType()->getIntegerBitWidth(), 0);
      if (callee.excludes(divisor, zero, *div))
        continue;
      auto alternatives = values(divisor, 0, &call);
      if (std::find(alternatives.begin(), alternatives.end(), zero) ==
          alternatives.end())
        continue;
      std::vector<const Value *> formal_leaves;
      callee.leaves(divisor, formal_leaves);
      std::vector<const Instruction *> evidence{div};
      bool returned_value = false;
      for (const Value *formal : formal_leaves)
        if (const auto *argument = dyn_cast<Argument>(formal))
          if (argument->getParent() == target &&
              argument->getArgNo() < call.arg_size()) {
            std::vector<const Value *> actual_leaves;
            engine(*call.getFunction())
                .leaves(*call.getArgOperand(argument->getArgNo()),
                        actual_leaves);
            for (const Value *actual : actual_leaves)
              if (const auto *source = dyn_cast<CallBase>(actual)) {
                returned_value = true;
                evidence.push_back(source);
              }
          }
      if (returned_value)
        report("cpp/divide-by-zero-using-return-value", call,
               "A function return alternative passed to this call makes "
               "arithmetic in the callee divide by zero.",
               evidence);
    }
  }

  void overflow(const BinaryOperator &op, Engine &facts) {
    if (!op.getType()->isIntegerTy() || !arithmetic(op.getOpcode()) ||
        op.getType()->getIntegerBitWidth() > 128)
      return;
    bool sign = facts.signedness(op).getValueOr(op.hasNoSignedWrap());
    if (!facts.mayOverflow(op, sign))
      return;
    std::vector<const Value *> leaves;
    facts.leaves(op, leaves);
    bool extreme = false, random = false;
    for (const Value *leaf : leaves) {
      if (const auto *c = dyn_cast<ConstantInt>(leaf)) {
        unsigned bits = c->getBitWidth();
        extreme |= c->getValue() == APInt::getSignedMinValue(bits) ||
                   c->getValue() == APInt::getSignedMaxValue(bits) ||
                   c->getValue() == APInt::getMaxValue(bits);
      }
      if (const auto *call = dyn_cast<CallBase>(leaf))
        if (const Function *target = ValueFacts::callee(*call)) {
          bool standard_rand =
              target->arg_empty() &&
              ValueFacts::hasLibraryName(*target, "rand", true, true);
          bool reentrant_rand = target->arg_size() == 1 &&
                                ValueFacts::hasLibraryName(*target, "rand_r");
          bool unix_random = target->arg_size() <= 1 &&
                             ValueFacts::hasLibraryName(*target, "random");
          random |= target->getReturnType()->isIntegerTy() &&
                    (standard_rand || reentrant_rand || unix_random);
        }
    }
    if (extreme)
      report("cpp/arithmetic-with-extreme-values", op,
             "A type-extreme value reaches arithmetic that may exceed its "
             "machine range.");
    if (random && (sign || op.getOpcode() == Instruction::Sub))
      report("cpp/uncontrolled-arithmetic", op,
             "Random data reaches arithmetic without bounds sufficient to "
             "prevent overflow.");
    if (!taint)
      return;
    bool controlled = false;
    std::vector<const Instruction *> origins;
    for (const Value *operand : op.operands())
      for (const TaintOrigin &origin : taint->origins(*operand))
        if (!origin.nonconstant_only) {
          controlled = true;
          if (const auto *site = dyn_cast_or_null<Instruction>(origin.source))
            origins.push_back(site);
        }
    if (controlled) {
      report("cpp/tainted-arithmetic", op,
             "User-controlled data reaches arithmetic which can exceed its "
             "machine range.",
             origins);
      report("cpp/integer-overflow-tainted", op,
             "The user-controlled arithmetic result may overflow before it is "
             "used.",
             origins);
    }
  }

  void conversion(const TruncInst &cast, Engine &facts) {
    if (!cast.getDestTy()->isIntegerTy() || !cast.getSrcTy()->isIntegerTy())
      return;
    const auto *op = dyn_cast<BinaryOperator>(cast.getOperand(0));
    if (op && arithmetic(op->getOpcode()) &&
        op->getType()->getIntegerBitWidth() <= 128) {
      bool sign = op->hasNoSignedWrap();
      unsigned bits = cast.getDestTy()->getIntegerBitWidth();
      if (!facts.mathematical(*op, sign, cast).subset(full(bits, sign))) {
        std::vector<const Value *> leaves;
        facts.leaves(*op, leaves);
        bool extreme = false;
        for (const Value *leaf : leaves)
          if (const auto *c = dyn_cast<ConstantInt>(leaf))
            if (c->getBitWidth() == bits)
              extreme |= c->getValue() == APInt::getSignedMinValue(bits) ||
                         c->getValue() == APInt::getSignedMaxValue(bits) ||
                         c->getValue() == APInt::getMaxValue(bits);
        if (extreme)
          report("cpp/arithmetic-with-extreme-values", cast,
                 "Arithmetic on a type-extreme value overflows when stored "
                 "back into the narrower integer type.",
                 {op});
      }
    }
    if (!taint)
      return;
    auto origins = taint->origins(*cast.getOperand(0));
    if (std::none_of(
            origins.begin(), origins.end(),
            [](const TaintOrigin &origin) { return !origin.nonconstant_only; }))
      return;
    // Without source signedness, prove loss only when neither interpretation
    // fits; sext/zext conversions and debug AST semantics are not guessed.
    unsigned bits = cast.getDestTy()->getIntegerBitWidth();
    bool signed_fits =
        facts.range(*cast.getOperand(0), true, cast).subset(full(bits, true));
    bool unsigned_fits =
        facts.range(*cast.getOperand(0), false, cast).subset(full(bits, false));
    if (!signed_fits && !unsigned_fits)
      report("cpp/integer-overflow-tainted", cast,
             "Conversion of the user-controlled value may lose integer bits.");
  }
};
} // namespace

const std::vector<AnalysisRuleDescriptor> &ArithmeticAnalysis::catalog() {
  static const std::vector<AnalysisRuleDescriptor> rules = {
      {"cpp/bad-addition-overflow-check", "error",
       "Likely Bugs/Arithmetic/BadAdditionOverflowCheck.ql",
       "Integer relational comparisons of an addition with an equivalent "
       "operand; both inputs widen and their complete type ranges fit the "
       "promoted addition."},
      {"cpp/unsigned-difference-expression-compared-zero", "warning",
       "Security/CWE/CWE-191/UnsignedDifferenceExpressionComparedZero.ql",
       "Unsigned difference compared with zero; excludes dominating "
       "operand-order guards, identical operands, masks, right shifts and "
       "constant unsigned division. AST expressions folded away before LLVM IR "
       "are unavailable."},
      {"cpp/multiplication-overflow-in-alloc", "warning",
       "experimental/Security/CWE/CWE-190/AllocMultiplicationOverflow.ql",
       "Allocation-size provenance through SSA, casts, arithmetic and "
       "must-alias MemorySSA spills; inferred allocation-size parameter roles "
       "of defined wrappers. Both product operands nonconstant and "
       "mathematical bounds permit overflow. Returned products across calls "
       "and unknown custom allocator declarations are not included."},
      {"cpp/if-statement-addition-overflow", "warning",
       "experimental/Security/CWE/CWE-190/IfStatementAdditionOverflow.ql",
       "Integer CFG guards with a+b versus c followed by a=c-b in a controlled "
       "block; equivalent loads require unchanged memory. SSA phi assignment "
       "and floating comparisons are not included."},
      {"cpp/integer-multiplication-cast-to-long", "warning",
       "Likely Bugs/Arithmetic/IntMultToLong.ql",
       "Integer products widened after arithmetic with at least two "
       "nonconstant operands larger than i8; range-safe products excluded. "
       "LLVM IR does not retain implicit-versus-explicit cast or macro "
       "distinctions."},
      {"cpp/divide-by-zero-using-return-value", "warning",
       "experimental/Security/CWE/CWE-369/DivideByZeroUsingReturnValue.ql",
       "Integer division/remainder with demonstrated constant return "
       "alternatives or standard zero-returning functions, local spills and "
       "constant add/subtract/multiply transformations. Direct caller-argument "
       "substitution and guards in caller/callee are supported; deeper "
       "argument "
       "contexts and floating division are not included."},
      {"cpp/arithmetic-with-extreme-values", "warning",
       "Security/CWE/CWE-190/ArithmeticWithExtremeValues.ql",
       "Type-extreme integer constants propagated through SSA and must-alias "
       "MemorySSA definitions to potentially overflowing arithmetic. Macro "
       "identity and interprocedural extreme-value flow are not retained."},
      {"cpp/uncontrolled-arithmetic", "warning",
       "Security/CWE/CWE-190/ArithmeticUncontrolled.ql",
       "rand/rand_r/random return flow through local SSA and must-alias spills "
       "into signed overflow or unsigned subtraction. Numeric range and "
       "dominating guards suppress safe arithmetic; rand_s output and "
       "cross-function random provenance are not included."},
      {"cpp/tainted-arithmetic", "warning",
       "Security/CWE/CWE-190/ArithmeticTainted.ql",
       "PDG scalar taint origins reach integer add/subtract/multiply/shift "
       "with mathematical ranges that exceed the result type. Guard sufficient "
       "bounds are honored; AST source signedness and source-level barriers "
       "may differ."},
      {"cpp/integer-overflow-tainted", "warning",
       "Security/CWE/CWE-190/IntegerOverflowTainted.ql",
       "PDG scalar taint origins reach overflowing integer arithmetic or lossy "
       "integer truncation. Uses CFG guards and mathematical range bounds; "
       "source-level conversion signedness is conservative."}};
  return rules;
}

bool ArithmeticAnalysis::requiresTaint(const std::string &id) {
  return id == "cpp/tainted-arithmetic" || id == "cpp/integer-overflow-tainted";
}

ArithmeticAnalysisResult
ArithmeticAnalysis::analyze(const Module &module,
                         const TaintFlowResult *taint) const {
  Queries queries(taint);
  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    Engine &facts = queries.engine(function);
    for (const Instruction &inst : instructions(function)) {
      if (const auto *cmp = dyn_cast<ICmpInst>(&inst))
        queries.comparison(*cmp, facts);
      if (const auto *store = dyn_cast<StoreInst>(&inst))
        queries.assignment(*store, facts);
      if (const auto *call = dyn_cast<CallBase>(&inst))
        queries.allocation(*call, facts);
      if (const auto *call = dyn_cast<CallBase>(&inst))
        queries.forwardedDivision(*call);
      if (const auto *cast = dyn_cast<CastInst>(&inst))
        queries.widening(*cast, facts);
      if (const auto *cast = dyn_cast<TruncInst>(&inst))
        queries.conversion(*cast, facts);
      if (const auto *op = dyn_cast<BinaryOperator>(&inst)) {
        queries.division(*op, facts);
        queries.overflow(*op, facts);
      }
    }
  }
  return std::move(queries.result);
}
} // namespace pdg
