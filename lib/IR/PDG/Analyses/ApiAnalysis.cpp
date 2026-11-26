#include "IR/PDG/Analyses/ApiAnalysis.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include "IR/PDG/Models/FunctionFacts.h"
#include "IR/PDG/Models/ValueFacts.h"

#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

using namespace llvm;

namespace pdg {
namespace {

std::string name(const CallBase &call) {
  const Function *f = ValueFacts::callee(call);
  return f ? ValueFacts::functionBaseName(*f) : std::string();
}

std::string qualifiedName(const CallBase &call) {
  const Function *f = ValueFacts::callee(call);
  return f ? demangle(f->getName().str()) : std::string();
}

bool globalCall(const CallBase &call, StringRef expected) {
  const Function *f = ValueFacts::callee(call);
  return f && ValueFacts::hasLibraryName(*f, expected.str(), true);
}

// MemorySSA gives a unique reaching definition, rather than looking for a
// convenient earlier store. Unknown calls and overlapping writes prevent
// propagation through mutable memory.
class Facts {
public:
  explicit Facts(const Function &function)
      : function_(const_cast<Function &>(function)),
        tli_impl_(Triple(function.getParent()->getTargetTriple())),
        tli_(tli_impl_), assumptions_(function_), dominators_(function_),
        basic_aa_(function.getParent()->getDataLayout(), function_, tli_,
                  assumptions_, &dominators_),
        aliases_(tli_), facts_(function_) {
    aliases_.addAAResult(basic_aa_);
    memory_ = std::make_unique<MemorySSA>(function_, &aliases_, &dominators_);
  }

  const Value *resolve(const Value *v, unsigned depth = 0) const {
    if (!v || depth > 24)
      return v;
    if (const auto *cast = dyn_cast<CastInst>(v)) {
      if (cast->isNoopCast(function_.getParent()->getDataLayout()) ||
          cast->getOpcode() == Instruction::BitCast)
        return resolve(cast->getOperand(0), depth + 1);
    }
    if (const auto *gep = dyn_cast<GEPOperator>(v)) {
      if (gep->hasAllZeroIndices())
        return resolve(gep->getPointerOperand(), depth + 1);
    }
    if (const auto *load = dyn_cast<LoadInst>(v)) {
      if (load->isVolatile() || load->isAtomic())
        return v;
      const auto *access =
          memory_->getWalker()->getClobberingMemoryAccess(load);
      const auto *def = dyn_cast_or_null<MemoryDef>(access);
      const auto *store =
          def ? dyn_cast_or_null<StoreInst>(def->getMemoryInst()) : nullptr;
      if (store && !store->isVolatile() && !store->isAtomic() &&
          store->getValueOperand()->getType() == load->getType() &&
          dominators_.dominates(store, load) &&
          aliases_.alias(MemoryLocation::get(store),
                         MemoryLocation::get(load)) == AliasResult::MustAlias)
        return resolve(store->getValueOperand(), depth + 1);
      const auto *global = dyn_cast<GlobalVariable>(
          load->getPointerOperand()->stripPointerCasts());
      if (global && global->isConstant() && global->hasInitializer())
        return resolve(global->getInitializer(), depth + 1);
    }
    if (const auto *select = dyn_cast<SelectInst>(v)) {
      if (auto c = integer(select->getCondition(), depth + 1))
        return resolve(c->isZero() ? select->getFalseValue()
                                   : select->getTrueValue(),
                       depth + 1);
      const Value *a = resolve(select->getTrueValue(), depth + 1);
      if (a == resolve(select->getFalseValue(), depth + 1))
        return a;
    }
    if (const auto *phi = dyn_cast<PHINode>(v)) {
      const Value *result = nullptr;
      for (const Value *incoming : phi->incoming_values()) {
        if (incoming == v)
          continue;
        const Value *item = resolve(incoming, depth + 1);
        if (result && result != item)
          return v;
        result = item;
      }
      if (result)
        return result;
    }
    if (const auto *call = dyn_cast<CallBase>(v)) {
      const Function *target = ValueFacts::callee(*call);
      if (target && !target->isDeclaration() && target != &function_) {
        const Value *result = nullptr;
        Facts nested(*target);
        for (const Instruction &inst : instructions(target)) {
          const auto *ret = dyn_cast<ReturnInst>(&inst);
          if (!ret || !ret->getReturnValue())
            continue;
          const Value *item = nested.resolve(ret->getReturnValue(), depth + 1);
          if (const auto *arg = dyn_cast<Argument>(item)) {
            if (arg->getArgNo() >= call->arg_size())
              return v;
            item = resolve(call->getArgOperand(arg->getArgNo()), depth + 1);
          } else if (!isa<Constant>(item)) {
            return v;
          }
          if (result && result != item)
            return v;
          result = item;
        }
        if (result)
          return result;
      }
    }
    return v;
  }

  Optional<APInt> integer(const Value *v, unsigned depth = 0) const {
    if (!v || depth > 24)
      return None;
    const Value *r = resolve(v, depth + 1);
    if (const auto *constant = dyn_cast<ConstantInt>(r))
      return constant->getValue();
    if (const auto *cast = dyn_cast<CastInst>(r)) {
      auto value = integer(cast->getOperand(0), depth + 1);
      if (!value || !cast->getType()->isIntegerTy())
        return None;
      unsigned width = cast->getType()->getIntegerBitWidth();
      return cast->getOpcode() == Instruction::SExt ? value->sextOrTrunc(width)
                                                    : value->zextOrTrunc(width);
    }
    if (const auto *cmp = dyn_cast<ICmpInst>(r)) {
      auto a = integer(cmp->getOperand(0), depth + 1);
      auto b = integer(cmp->getOperand(1), depth + 1);
      if (!a || !b || a->getBitWidth() != b->getBitWidth())
        return None;
      bool result;
      switch (cmp->getPredicate()) {
      case ICmpInst::ICMP_EQ:
        result = *a == *b;
        break;
      case ICmpInst::ICMP_NE:
        result = *a != *b;
        break;
      case ICmpInst::ICMP_ULT:
        result = a->ult(*b);
        break;
      case ICmpInst::ICMP_ULE:
        result = a->ule(*b);
        break;
      case ICmpInst::ICMP_UGT:
        result = a->ugt(*b);
        break;
      case ICmpInst::ICMP_UGE:
        result = a->uge(*b);
        break;
      case ICmpInst::ICMP_SLT:
        result = a->slt(*b);
        break;
      case ICmpInst::ICMP_SLE:
        result = a->sle(*b);
        break;
      case ICmpInst::ICMP_SGT:
        result = a->sgt(*b);
        break;
      case ICmpInst::ICMP_SGE:
        result = a->sge(*b);
        break;
      default:
        return None;
      }
      return APInt(1, result);
    }
    if (const auto *op = dyn_cast<BinaryOperator>(r)) {
      auto a = integer(op->getOperand(0), depth + 1);
      auto b = integer(op->getOperand(1), depth + 1);
      if (!a || !b || a->getBitWidth() != b->getBitWidth())
        return None;
      switch (op->getOpcode()) {
      case Instruction::Add:
        return *a + *b;
      case Instruction::Sub:
        return *a - *b;
      case Instruction::Mul:
        return *a * *b;
      case Instruction::Or:
        return *a | *b;
      case Instruction::And:
        return *a & *b;
      case Instruction::Xor:
        return *a ^ *b;
      default:
        return None;
      }
    }
    if (const auto *call = dyn_cast<CallBase>(r)) {
      const Function *target = ValueFacts::callee(*call);
      if (target && !target->isDeclaration() && target != &function_) {
        Optional<APInt> result;
        bool returned = false;
        for (const Instruction &inst : instructions(target)) {
          const auto *ret = dyn_cast<ReturnInst>(&inst);
          if (!ret || !ret->getReturnValue())
            continue;
          const Value *value = ret->getReturnValue();
          Facts nested(*target);
          value = nested.resolve(value);
          Optional<APInt> item;
          if (const auto *arg = dyn_cast<Argument>(value)) {
            if (arg->getArgNo() < call->arg_size())
              item = integer(call->getArgOperand(arg->getArgNo()), depth + 1);
          } else {
            item = nested.integer(value, depth + 1);
          }
          if (!item || (returned && *result != *item))
            return None;
          result = item;
          returned = true;
        }
        return result;
      }
    }
    return None;
  }

  bool same(const Value *a, const Value *b) const {
    return a && b && (resolve(a) == resolve(b) || facts_.equivalent(*a, *b));
  }

  bool zero(const Value *v) const {
    v = resolve(v);
    if (isa<ConstantPointerNull>(v))
      return true;
    auto i = integer(v);
    return i && i->isZero();
  }

  DominatorTree &dominators() { return dominators_; }

private:
  Function &function_;
  TargetLibraryInfoImpl tli_impl_;
  TargetLibraryInfo tli_;
  AssumptionCache assumptions_;
  mutable DominatorTree dominators_;
  BasicAAResult basic_aa_;
  mutable AAResults aliases_;
  std::unique_ptr<MemorySSA> memory_;
  FunctionFacts facts_;
};

struct Engine {
  ApiAnalysisResult result;
  std::map<const Function *, std::unique_ptr<Facts>> functions;
  std::set<std::pair<std::string, const Instruction *>> emitted;
  std::set<const Function *> verify_returns;
  std::set<const Argument *> verify_arguments;

  Facts &facts(const Function &f) {
    auto &entry = functions[&f];
    if (!entry)
      entry = std::make_unique<Facts>(f);
    return *entry;
  }

  void emit(StringRef id, const Instruction &site, StringRef message,
            std::vector<const Instruction *> evidence = {}) {
    if (emitted.emplace(id.str(), &site).second)
      result.findings.push_back({id.str(), &site, message.str(), evidence});
  }

  bool verifyValue(const Value *value, const Function &f, unsigned depth = 0) {
    if (!value || depth > 24)
      return false;
    value = facts(f).resolve(value);
    if (const auto *a = dyn_cast<Argument>(value))
      return verify_arguments.count(a);
    if (const auto *call = dyn_cast<CallBase>(value))
      return globalCall(*call, "SSL_get_verify_result") ||
             verify_returns.count(ValueFacts::callee(*call));
    if (const auto *cast = dyn_cast<CastInst>(value))
      return verifyValue(cast->getOperand(0), f, depth + 1);
    if (const auto *phi = dyn_cast<PHINode>(value)) {
      for (const Value *incoming : phi->incoming_values())
        if (incoming != value && verifyValue(incoming, f, depth + 1))
          return true;
    }
    return false;
  }

  void inferVerifyFlow(const Module &module) {
    bool changed;
    do {
      changed = false;
      for (const Function &f : module) {
        if (f.isDeclaration())
          continue;
        for (const Instruction &inst : instructions(f)) {
          if (const auto *ret = dyn_cast<ReturnInst>(&inst)) {
            if (verifyValue(ret->getReturnValue(), f))
              changed |= verify_returns.insert(&f).second;
          }
          const auto *call = dyn_cast<CallBase>(&inst);
          const Function *target = call ? ValueFacts::callee(*call) : nullptr;
          if (!target || target->isDeclaration())
            continue;
          for (const Argument &arg : target->args())
            if (arg.getArgNo() < call->arg_size() &&
                verifyValue(call->getArgOperand(arg.getArgNo()), f))
              changed |= verify_arguments.insert(&arg).second;
        }
      }
    } while (changed);
  }

  // Executes the control-flow edges after a protocol source. The state is
  // finite, and each (instruction,state) pair is visited once. Constant branch
  // conditions are respected; null-certificate edges terminate that protocol.
  template <typename Step, typename Edge>
  void walk(const Instruction &start, unsigned initial, Step step, Edge edge,
            bool include_start = false) {
    using Constraint = std::tuple<const Value *, std::string, bool>;
    using Constraints = std::set<Constraint>;
    using Point = std::tuple<const Instruction *, unsigned, Constraints>;
    Facts &initial_facts = facts(*start.getFunction());
    Constraints initial_constraints;
    for (const Instruction &inst : instructions(start.getFunction())) {
      const auto *branch = dyn_cast<BranchInst>(&inst);
      const auto *cmp = branch && branch->isConditional()
                            ? dyn_cast<ICmpInst>(
                                  initial_facts.resolve(branch->getCondition()))
                            : nullptr;
      if (!cmp || !cmp->isEquality())
        continue;
      for (unsigned successor = 0; successor < 2; ++successor) {
        if (!initial_facts.dominators().dominates(
                BasicBlockEdge(branch->getParent(),
                               branch->getSuccessor(successor)),
                start.getParent()))
          continue;
        for (unsigned operand = 0; operand < 2; ++operand) {
          auto constant = initial_facts.integer(cmp->getOperand(1 - operand));
          const Value *value = initial_facts.resolve(cmp->getOperand(operand));
          if (!constant || initial_facts.integer(value))
            continue;
          SmallString<40> text;
          constant->toString(text, 10, true);
          std::string encoded =
              std::to_string(constant->getBitWidth()) + ":" + text.str().str();
          bool equal =
              (successor == 0) == (cmp->getPredicate() == ICmpInst::ICMP_EQ);
          initial_constraints.emplace(value, encoded, equal);
        }
      }
    }
    std::deque<Point> pending;
    if (include_start)
      pending.emplace_back(&start, initial, initial_constraints);
    else if (start.getNextNode())
      pending.emplace_back(start.getNextNode(), initial, initial_constraints);
    else if (const auto *invoke = dyn_cast<InvokeInst>(&start))
      pending.emplace_back(&invoke->getNormalDest()->front(), initial,
                           initial_constraints);
    std::set<Point> visited;
    while (!pending.empty()) {
      auto item = pending.front();
      pending.pop_front();
      const Instruction *inst = std::get<0>(item);
      unsigned state = std::get<1>(item);
      const Constraints &constraints = std::get<2>(item);
      if (!visited.insert(item).second)
        continue;
      if (!step(*inst, state))
        continue;
      if (!inst->isTerminator()) {
        if (inst->getNextNode())
          pending.emplace_back(inst->getNextNode(), state, constraints);
        continue;
      }
      const auto *branch = dyn_cast<BranchInst>(inst);
      Optional<APInt> condition;
      if (branch && branch->isConditional())
        condition = facts(*inst->getFunction()).integer(branch->getCondition());
      Optional<unsigned> selected_successor;
      if (const auto *sw = dyn_cast<SwitchInst>(inst)) {
        if (auto value =
                facts(*inst->getFunction()).integer(sw->getCondition())) {
          selected_successor = 0;
          for (const auto &item : sw->cases())
            if (item.getCaseValue()->getValue() == *value)
              selected_successor = item.getSuccessorIndex();
        }
      }
      for (unsigned i = 0; i < inst->getNumSuccessors(); ++i) {
        if (condition && i != (condition->isZero() ? 1u : 0u))
          continue;
        if (selected_successor && i != *selected_successor)
          continue;
        if (!edge(*inst, i, state))
          continue;
        Constraints next_constraints = constraints;
        Facts &fa = facts(*inst->getFunction());
        // A repeated instruction in a loop may produce a fresh value. Do
        // not carry prior-iteration result constraints across back edges.
        if (fa.dominators().dominates(inst->getSuccessor(i), inst->getParent()))
          next_constraints.clear();
        const auto *cmp =
            branch && branch->isConditional()
                ? dyn_cast<ICmpInst>(fa.resolve(branch->getCondition()))
                : nullptr;
        bool feasible = true;
        if (cmp && cmp->isEquality()) {
          for (unsigned operand = 0; operand < 2; ++operand) {
            auto constant = fa.integer(cmp->getOperand(1 - operand));
            const Value *value = fa.resolve(cmp->getOperand(operand));
            if (!constant || fa.integer(value))
              continue;
            SmallString<40> text;
            constant->toString(text, 10, true);
            std::string encoded = std::to_string(constant->getBitWidth()) +
                                  ":" + text.str().str();
            bool equal = (i == 0) == (cmp->getPredicate() == ICmpInst::ICMP_EQ);
            for (const Constraint &previous : next_constraints) {
              if (std::get<0>(previous) != value)
                continue;
              bool same_constant = std::get<1>(previous) == encoded;
              if ((equal && std::get<2>(previous) && !same_constant) ||
                  (same_constant && equal != std::get<2>(previous)))
                feasible = false;
            }
            if (feasible)
              next_constraints.emplace(value, encoded, equal);
          }
        }
        if (feasible)
          pending.emplace_back(&inst->getSuccessor(i)->front(), state,
                               std::move(next_constraints));
      }
    }
  }

  void certificate(const CallBase &source) {
    if (!source.arg_size())
      return;
    Facts &fa = facts(*source.getFunction());
    walk(
        source, 0,
        [&](const Instruction &inst, unsigned &) {
          if (const auto *call = dyn_cast<CallBase>(&inst)) {
            if (globalCall(*call, "SSL_get_verify_result") &&
                call->arg_size() &&
                fa.same(source.getArgOperand(0), call->getArgOperand(0)))
              return false;
          }
          if (isa<ReturnInst>(inst))
            emit("cpp/certificate-not-checked", source,
                 "A path returns with a retrieved certificate without checking "
                 "the same SSL object's verification result.",
                 {&inst});
          return true;
        },
        [&](const Instruction &inst, unsigned successor, unsigned) {
          const auto *branch = dyn_cast<BranchInst>(&inst);
          const auto *cmp =
              branch && branch->isConditional()
                  ? dyn_cast<ICmpInst>(fa.resolve(branch->getCondition()))
                  : nullptr;
          if (!cmp || !cmp->isEquality())
            return true;
          bool matches = (fa.same(cmp->getOperand(0), &source) &&
                          fa.zero(cmp->getOperand(1))) ||
                         (fa.same(cmp->getOperand(1), &source) &&
                          fa.zero(cmp->getOperand(0)));
          if (!matches)
            return true;
          bool null_edge =
              successor == (cmp->getPredicate() == ICmpInst::ICMP_EQ ? 0u : 1u);
          return !null_edge;
        });
  }

  void shutdown(const CallBase &source) {
    if (!source.arg_size())
      return;
    Facts &fa = facts(*source.getFunction());
    walk(
        source, 0,
        [&](const Instruction &inst, unsigned &) {
          const auto *call = dyn_cast<CallBase>(&inst);
          if (!call || !call->arg_size())
            return true;
          if (globalCall(*call, "SSL_free") &&
              fa.same(source.getArgOperand(0), call->getArgOperand(0)))
            return false;
          if (globalCall(*call, "SSL_shutdown") && call != &source &&
              call->use_empty() &&
              fa.same(source.getArgOperand(0), call->getArgOperand(0)))
            emit("cpp/dangerous-use-of-ssl-shutdown", source,
                 "A repeated SSL_shutdown on the same live SSL object discards "
                 "its result.",
                 {call});
          return true;
        },
        [](const Instruction &, unsigned, unsigned) { return true; });
  }

  bool xmlType(const CallBase &call) {
    std::string storage = qualifiedName(call);
    StringRef qualified(storage);
    return qualified.contains("AbstractDOMParser::") ||
           qualified.contains("XercesDOMParser::") ||
           qualified.contains("SAXParser::") ||
           qualified.contains("SAX2XMLReader::") ||
           qualified.contains("DOMLSParser::");
  }

  void xmlObject(const CallBase &source, const Value *object) {
    // Each bit in the state set represents one configuration (disable
    // resolution, create reference nodes). A helper's input/output summary is
    // keyed by its formal object and state set, preserving matched returns.
    using SummaryKey = std::tuple<const Function *, const Value *, unsigned>;
    std::map<SummaryKey, unsigned> summaries;
    std::set<SummaryKey> active;
    std::function<unsigned(const Instruction &, const Value *, unsigned, bool)>
        run;
    run = [&](const Instruction &start, const Value *tracked, unsigned initial,
              bool include_start) {
      const Function &f = *start.getFunction();
      SummaryKey key{&f, tracked, initial};
      if (include_start) {
        auto found = summaries.find(key);
        if (found != summaries.end())
          return found->second;
        if (!active.insert(key).second)
          return 0u; // Recursive object protocols have no summary yet.
      }
      unsigned output = 0;
      Facts &fa = facts(f);
      walk(
          start, initial,
          [&](const Instruction &inst, unsigned &states) {
            if (isa<ReturnInst>(inst)) {
              output |= states;
              return false;
            }
            const auto *call = dyn_cast<CallBase>(&inst);
            if (!call || !call->arg_size())
              return true;
            std::string base = name(*call);
            const Value *receiver = call->getArgOperand(0);
            bool same = fa.same(receiver, tracked);
            if (base == "setParameter") {
              const auto *configuration =
                  dyn_cast<CallBase>(fa.resolve(receiver));
              same = configuration && name(*configuration) == "getDomConfig" &&
                     configuration->arg_size() && xmlType(*configuration) &&
                     fa.same(configuration->getArgOperand(0), tracked);
            }
            if (same && base == "parse" && xmlType(*call)) {
              if (states & ~8u)
                emit(
                    "cpp/external-entity-expansion", inst,
                    "This XML parser can parse with external entity resolution "
                    "enabled.",
                    {&source});
              return true;
            }
            unsigned bit = 0;
            const Value *setting = nullptr;
            if (same && base == "setDisableDefaultEntityResolution" &&
                xmlType(*call) && call->arg_size() >= 2) {
              bit = 1;
              setting = call->getArgOperand(1);
            } else if (same && base == "setCreateEntityReferenceNodes" &&
                       xmlType(*call) && call->arg_size() >= 2) {
              bit = 2;
              setting = call->getArgOperand(1);
            } else if (same &&
                       (base == "setFeature" || base == "setParameter") &&
                       call->arg_size() >= 3) {
              const Value *feature = fa.resolve(call->getArgOperand(1));
              const auto *global =
                  dyn_cast<GlobalValue>(feature->stripPointerCasts());
              if (global &&
                  demangle(global->getName().str())
                          .find("XMLUni::"
                                "fgXercesDisableDefaultEntityResolution") !=
                      std::string::npos) {
                bit = 1;
                setting = call->getArgOperand(2);
              }
            }
            if (setting) {
              auto value = fa.integer(setting);
              unsigned transformed = 0;
              for (unsigned state = 0; state < 4; ++state)
                if (states & (1u << state)) {
                  unsigned next =
                      value && *value == 1 ? state | bit : state & ~bit;
                  transformed |= 1u << next;
                }
              states = transformed;
            } else if (const Function *target = ValueFacts::callee(*call)) {
              if (!target->isDeclaration() && !target->empty()) {
                for (unsigned i = 0;
                     i < call->arg_size() && i < target->arg_size(); ++i) {
                  if (!fa.same(call->getArgOperand(i), tracked))
                    continue;
                  states = run(target->getEntryBlock().front(),
                               target->getArg(i), states, true);
                  break;
                }
              }
            }
            return states != 0;
          },
          [](const Instruction &, unsigned, unsigned) { return true; },
          include_start);
      if (include_start) {
        active.erase(key);
        summaries.emplace(key, output);
      }
      return output;
    };
    run(source, object, 1u << 2, false);
  }

  // A short-circuit region accepts two different equality values on one
  // outcome. Dominance of the continuation excludes two independent tests.
  void conflation(const Function &f) {
    struct Test {
      const ICmpInst *compare;
      const BranchInst *branch;
      const Value *value;
      APInt constant;
    };
    std::vector<Test> tests;
    Facts &fa = facts(f);
    for (const Instruction &inst : instructions(f)) {
      const auto *branch = dyn_cast<BranchInst>(&inst);
      const auto *cmp =
          branch && branch->isConditional()
              ? dyn_cast<ICmpInst>(fa.resolve(branch->getCondition()))
              : nullptr;
      if (!cmp || !cmp->isEquality())
        continue;
      for (unsigned op = 0; op < 2; ++op) {
        auto constant = fa.integer(cmp->getOperand(1 - op));
        if (constant && verifyValue(cmp->getOperand(op), f))
          tests.push_back(
              {cmp, branch, fa.resolve(cmp->getOperand(op)), *constant});
      }
    }
    auto destination = [](const BasicBlock *block) {
      const auto *branch = dyn_cast<BranchInst>(block->getTerminator());
      return branch && branch->isUnconditional() ? branch->getSuccessor(0)
                                                 : block;
    };
    for (const auto &a : tests) {
      if (!a.constant.isZero())
        continue;
      for (const auto &b : tests) {
        if (b.constant.isZero() ||
            a.compare->getPredicate() != b.compare->getPredicate() ||
            !fa.same(a.value, b.value))
          continue;
        unsigned accepted =
            a.compare->getPredicate() == ICmpInst::ICMP_EQ ? 0u : 1u;
        if (fa.dominators().dominates(
                BasicBlockEdge(a.branch->getParent(),
                               a.branch->getSuccessor(1 - accepted)),
                b.compare->getParent()) &&
            destination(a.branch->getSuccessor(accepted)) ==
                destination(b.branch->getSuccessor(accepted)))
          emit("cpp/certificate-result-conflation", *a.compare,
               "This guard accepts both a successful and an unsuccessful "
               "certificate verification result.",
               {b.compare});
      }
    }
    std::set<const PHINode *> conflated_values;
    for (const Instruction &inst : instructions(f)) {
      const auto *phi = dyn_cast<PHINode>(&inst);
      if (!phi || !phi->getType()->isIntegerTy(1) ||
          phi->getNumIncomingValues() != 2)
        continue;
      for (unsigned constant_index = 0; constant_index < 2; ++constant_index) {
        const auto *constant =
            dyn_cast<ConstantInt>(phi->getIncomingValue(constant_index));
        const auto *entry = dyn_cast<BranchInst>(
            phi->getIncomingBlock(constant_index)->getTerminator());
        const auto *a =
            entry && entry->isConditional()
                ? dyn_cast<ICmpInst>(fa.resolve(entry->getCondition()))
                : nullptr;
        const auto *b = dyn_cast<ICmpInst>(
            fa.resolve(phi->getIncomingValue(1 - constant_index)));
        if (!constant || !a || !b || !a->isEquality() ||
            a->getPredicate() != b->getPredicate())
          continue;
        unsigned accepted = a->getPredicate() == ICmpInst::ICMP_EQ ? 0u : 1u;
        if (constant->isZero() != (accepted == 1) ||
            entry->getSuccessor(accepted) != phi->getParent() ||
            !fa.dominators().dominates(
                BasicBlockEdge(entry->getParent(),
                               entry->getSuccessor(1 - accepted)),
                b->getParent()))
          continue;
        for (unsigned left = 0; left < 2; ++left) {
          auto ac = fa.integer(a->getOperand(1 - left));
          if (!ac || !verifyValue(a->getOperand(left), f))
            continue;
          for (unsigned right = 0; right < 2; ++right) {
            auto bc = fa.integer(b->getOperand(1 - right));
            if (bc && ac->isZero() != bc->isZero() &&
                fa.same(a->getOperand(left), b->getOperand(right))) {
              emit("cpp/certificate-result-conflation", *a,
                   "This Boolean expression accepts both successful and "
                   "unsuccessful certificate verification results.",
                   {b});
              conflated_values.insert(phi);
            }
          }
        }
      }
    }
    for (const Instruction &inst : instructions(f)) {
      const auto *branch = dyn_cast<BranchInst>(&inst);
      if (!branch || !branch->isConditional())
        continue;
      const Value *condition = fa.resolve(branch->getCondition());
      unsigned depth = 0;
      while (const auto *cast = dyn_cast<CastInst>(condition)) {
        if (++depth > 24)
          break;
        condition = fa.resolve(cast->getOperand(0));
      }
      if (const auto *phi = dyn_cast<PHINode>(condition))
        if (conflated_values.count(phi))
          emit("cpp/certificate-result-conflation", *branch,
               "This guard uses a Boolean that conflates successful and "
               "unsuccessful certificate verification results.",
               {phi});
    }
  }
};

bool arithmetic(const Value *value, Facts &facts, unsigned depth = 0) {
  if (!value || depth > 24)
    return false;
  value = facts.resolve(value);
  const auto *op = dyn_cast<BinaryOperator>(value);
  if (!op)
    return false;
  switch (op->getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::SDiv:
  case Instruction::UDiv:
  case Instruction::SRem:
  case Instruction::URem:
    return true;
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    return arithmetic(op->getOperand(0), facts, depth + 1) ||
           arithmetic(op->getOperand(1), facts, depth + 1);
  default:
    return false;
  }
}

bool weakAlgorithm(StringRef input) {
  std::string normalized;
  for (char c : input)
    normalized.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  StringRef text(normalized);
  for (StringRef token :
       {"des", "rc2", "rc4", "rc5", "arcfour", "3des", "des3"}) {
    size_t position = 0;
    while ((position = text.find(token, position)) != StringRef::npos) {
      bool left = position == 0 ||
                  !std::isalnum(static_cast<unsigned char>(text[position - 1]));
      size_t end = position + token.size();
      bool right = end == text.size() ||
                   !std::isalnum(static_cast<unsigned char>(text[end]));
      // CamelCase names preserve the explicit algorithm boundary in IR.
      left |= position &&
              std::islower(static_cast<unsigned char>(input[position - 1])) &&
              (std::isupper(static_cast<unsigned char>(input[position])) ||
               (std::isdigit(static_cast<unsigned char>(input[position])) &&
                position + 1 < input.size() &&
                std::isupper(static_cast<unsigned char>(input[position + 1]))));
      right |= end + 1 < input.size() &&
               std::isupper(static_cast<unsigned char>(input[end])) &&
               std::islower(static_cast<unsigned char>(input[end + 1]));
      if (left && right)
        return true;
      ++position;
    }
  }
  return false;
}

} // namespace

const std::vector<AnalysisRuleDescriptor> &ApiAnalysis::catalog() {
  static const std::vector<AnalysisRuleDescriptor> rules = {
      {"cpp/insufficient-key-size", "error",
       "Security/CWE/CWE-326/InsufficientKeySize.ql",
       "LLVM-visible positive constant key strengths below 2048 bits, "
       "including reaching stores and transparent constant/argument returns."},
      {"cpp/weak-cryptographic-algorithm", "error",
       "Security/CWE/CWE-327/BrokenCryptoAlgorithm.ql",
       "IR-visible algorithm calls or arguments with encryption-role evidence "
       "and nonconstant buffer/object arguments; source-only macros and enum "
       "names are unavailable."},
      {"cpp/certificate-result-conflation", "error",
       "Security/CWE/CWE-295/SSLResultConflation.ql",
       "Short-circuit equality guards on verification results, including "
       "transparent result wrappers and parameter forwarding; source guards "
       "erased from IR are unavailable."},
      {"cpp/certificate-not-checked", "error",
       "Security/CWE/CWE-295/SSLResultNotChecked.ql",
       "Intraprocedural executable paths after certificate retrieval, "
       "same-object verification barriers and null-certificate edge "
       "exclusions."},
      {"cpp/dangerous-use-of-ssl-shutdown", "warning",
       "experimental/Security/CWE/CWE-670/DangerousUseSSL_shutdown.ql",
       "Intraprocedural repeated shutdown of the same live object with a "
       "discarded second result; same-object SSL_free terminates the "
       "protocol."},
      {"cpp/unsafe-dacl-security-descriptor", "error",
       "Security/CWE/CWE-732/UnsafeDaclSecurityDescriptor.ql",
       "Nonzero DACL-present argument and a proven null DACL through SSA or a "
       "unique reaching memory definition."},
      {"cpp/external-entity-expansion", "warning",
       "Security/CWE/CWE-611/XXE.ql",
       "Libxml2 option-bit models and finite-state Xerces configuration "
       "protocols with matched object-parameter helper summaries; global "
       "parser state and recursive protocols remain unsupported."},
      {"cpp/wrong-use-of-the-umask", "warning",
       "experimental/Security/CWE/CWE-266/IncorrectPrivilegeAssignment.ql",
       "Permission arithmetic provenance, equal nonzero umask/chmod masks in a "
       "function, and zero-mask writable fopen paths without an intervening "
       "restrictive mask or chmod."}};
  return rules;
}

ApiAnalysisResult ApiAnalysis::analyze(const Module &module) const {
  Engine engine;
  engine.inferVerifyFlow(module);
  const std::set<std::string> key_apis = {
      "EVP_PKEY_CTX_set_dsa_paramgen_bits",
      "DSA_generate_parameters_ex",
      "EVP_PKEY_CTX_set_rsa_keygen_bits",
      "RSA_generate_key_ex",
      "RSA_generate_key_fips",
      "EVP_PKEY_CTX_set_dh_paramgen_prime_len",
      "DH_generate_parameters_ex"};
  const std::map<std::string, unsigned> xml_options = {
      {"xmlCtxtUseOptions", 1}, {"xmlReadFile", 2},
      {"xmlCtxtReadFile", 3},   {"xmlParseInNodeContext", 3},
      {"xmlReadDoc", 3},        {"xmlReadFd", 3},
      {"xmlCtxtReadDoc", 4},    {"xmlCtxtReadFd", 4},
      {"xmlReadMemory", 4},     {"xmlCtxtReadMemory", 5},
      {"xmlReadIO", 5},         {"xmlCtxtReadIO", 6}};
  for (const Function &f : module) {
    if (f.isDeclaration())
      continue;
    Facts &facts = engine.facts(f);
    engine.conflation(f);
    std::vector<const CallBase *> masks, permissions;
    for (const Instruction &inst : instructions(f)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;
      std::string base = name(*call);
      if (key_apis.count(base) && globalCall(*call, base) &&
          call->arg_size() >= 2) {
        auto strength = facts.integer(call->getArgOperand(1));
        if (strength && strength->isStrictlyPositive() && strength->ult(2048))
          engine.emit("cpp/insufficient-key-size", inst,
                      "The cryptographic key strength is below 2048 bits.");
      }
      if (globalCall(*call, "SetSecurityDescriptorDacl") &&
          call->arg_size() >= 3) {
        auto present = facts.integer(call->getArgOperand(1));
        if (present && !present->isZero() && facts.zero(call->getArgOperand(2)))
          engine.emit("cpp/unsafe-dacl-security-descriptor", inst,
                      "A present null DACL grants unrestricted access to the "
                      "protected object.");
      }
      if (globalCall(*call, "SSL_get_peer_certificate") ||
          globalCall(*call, "SSL_get1_peer_certificate"))
        engine.certificate(*call);
      if (globalCall(*call, "SSL_shutdown"))
        engine.shutdown(*call);
      auto options = xml_options.find(base);
      if (options != xml_options.end() && globalCall(*call, base) &&
          call->arg_size() > options->second) {
        auto flags = facts.integer(call->getArgOperand(options->second));
        if (flags && !(*flags & APInt(flags->getBitWidth(), 6)).isZero())
          engine.emit(
              "cpp/external-entity-expansion", inst,
              "Libxml2 parsing enables XML_PARSE_NOENT or XML_PARSE_DTDLOAD.");
      }
      if ((base == "XercesDOMParser" || base == "SAXParser") &&
          engine.xmlType(*call) && call->arg_size())
        engine.xmlObject(*call, call->getArgOperand(0));
      if (base == "createXMLReader" || base == "createLSParser") {
        std::string qualified = qualifiedName(*call);
        if (StringRef(qualified).contains("XMLReaderFactory::") ||
            StringRef(qualified).contains("DOMImplementationLS::"))
          engine.xmlObject(*call, call);
      }
      if (globalCall(*call, "umask") && call->arg_size())
        masks.push_back(call);
      if ((globalCall(*call, "chmod") || globalCall(*call, "fchmod")) &&
          call->arg_size() >= 2)
        permissions.push_back(call);
      unsigned permission_argument = base == "umask" ? 0 : 1;
      if ((globalCall(*call, "umask") || globalCall(*call, "chmod") ||
           globalCall(*call, "fchmod")) &&
          call->arg_size() > permission_argument &&
          arithmetic(call->getArgOperand(permission_argument), facts))
        engine.emit("cpp/wrong-use-of-the-umask", inst,
                    "Arithmetic computes a permission mask whose bits should "
                    "be combined with bitwise operations.");

      std::string qualified = qualifiedName(*call);
      std::string lower = StringRef(base).lower();
      bool encryption = StringRef(lower).contains("crypt") ||
                        StringRef(lower).contains("cipher") ||
                        StringRef(lower).contains("cbc") ||
                        StringRef(lower).contains("key");
      bool weak = weakAlgorithm(qualified);
      bool buffer = false;
      for (const Use &arg : call->args()) {
        const Value *v = facts.resolve(arg.get());
        buffer |= v->getType()->isPointerTy() && !isa<Constant>(v);
        if (encryption) {
          if (auto text = ValueFacts::constantString(*v))
            weak |= weakAlgorithm(*text);
          if (const auto *algorithm = dyn_cast<CallBase>(v))
            weak |= weakAlgorithm(qualifiedName(*algorithm));
        }
      }
      // Parameters with an explicit key type supply the additional evidence
      // used by the original query without attributing all methods of a
      // cipher-named class to encryption operations.
      size_t parameters = qualified.find('(');
      if (parameters != std::string::npos)
        encryption |=
            StringRef(qualified).drop_front(parameters).lower().find("key") !=
            std::string::npos;
      bool template_body = StringRef(demangle(f.getName().str())).contains("<");
      if (weak && encryption && buffer && !template_body &&
          !StringRef(base).startswith("~"))
        engine.emit("cpp/weak-cryptographic-algorithm", inst,
                    "The encryption operation selects a broken DES/RC-family "
                    "algorithm.");
    }
    for (const CallBase *mask : masks) {
      auto constant = facts.integer(mask->getArgOperand(0));
      for (const CallBase *permission : permissions)
        if ((!constant || !constant->isZero()) &&
            facts.same(mask->getArgOperand(0), permission->getArgOperand(1)))
          engine.emit("cpp/wrong-use-of-the-umask", *mask,
                      "The same nonzero bits are used as both a permission "
                      "mask and allowed chmod permissions.",
                      {permission});
      if (!constant || !constant->isZero())
        continue;
      engine.walk(
          *mask, 0,
          [&](const Instruction &inst, unsigned &) {
            const auto *call = dyn_cast<CallBase>(&inst);
            if (!call)
              return true;
            if (globalCall(*call, "umask") && call->arg_size()) {
              auto value = facts.integer(call->getArgOperand(0));
              if (!value || !value->isZero())
                return false;
            }
            if (globalCall(*call, "chmod") || globalCall(*call, "fchmod"))
              return false;
            if (globalCall(*call, "fopen") && call->arg_size() >= 2) {
              auto mode = ValueFacts::constantString(
                  *facts.resolve(call->getArgOperand(1)));
              auto path = ValueFacts::constantString(
                  *facts.resolve(call->getArgOperand(0)));
              if (mode &&
                  (mode->find('w') != std::string::npos ||
                   mode->find('a') != std::string::npos) &&
                  (!path || *path != "/dev/null"))
                engine.emit("cpp/wrong-use-of-the-umask", *mask,
                            "A zero umask leaves a created file accessible "
                            "without restrictive permission setup.",
                            {call});
            }
            return true;
          },
          [](const Instruction &, unsigned, unsigned) { return true; });
    }
  }
  return std::move(engine.result);
}

} // namespace pdg
