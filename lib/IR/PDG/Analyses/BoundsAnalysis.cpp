#include "IR/PDG/Analyses/BoundsAnalysis.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"

#include "IR/PDG/Analyses/TaintAnalysis.h"
#include "IR/PDG/Models/FunctionFacts.h"
#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Models/ValueFacts.h"

#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>

using namespace llvm;

namespace pdg {

bool BufferAccess::outOfBounds() const {
  // Zero-length operations need no dereference, including one-past pointers.
  if (!bytes)
    return false;
  if (region.offset < 0)
    return true;
  uint64_t offset = static_cast<uint64_t>(region.offset);
  return offset > region.capacity || bytes > region.capacity - offset;
}

namespace {
Optional<uint64_t> typeBytes(Type *type, const DataLayout &layout,
                             bool storage = true) {
  if (!type->isSized())
    return None;
  TypeSize size =
      storage ? layout.getTypeAllocSize(type) : layout.getTypeStoreSize(type);
  return size.isScalable() ? None : Optional<uint64_t>(size.getFixedValue());
}

bool characterArray(Type *type) {
  const auto *array = dyn_cast<ArrayType>(type);
  return array && array->getElementType()->isIntegerTy(8);
}

Optional<uint64_t> product(uint64_t a, uint64_t b) {
  if (a && b > std::numeric_limits<uint64_t>::max() / a)
    return None;
  return a * b;
}
} // namespace

struct BoundsAnalysis::Impl {
  struct FunctionState {
    Function &function;
    TargetLibraryInfoImpl tli_impl;
    TargetLibraryInfo tli;
    AssumptionCache assumptions;
    DominatorTree dominators;
    BasicAAResult basic_aa;
    AAResults aliases;
    std::unique_ptr<MemorySSA> memory;
    FunctionFacts facts;
    std::unique_ptr<LoopInfo> loops_;
    std::unique_ptr<ScalarEvolution> evolution_;

    ScalarEvolution &getEvolution() {
      if (!evolution_) {
        loops_ = std::make_unique<LoopInfo>(dominators);
        evolution_ = std::make_unique<ScalarEvolution>(
            function, tli, assumptions, dominators, *loops_);
      }
      return *evolution_;
    }

    explicit FunctionState(Function &f)
        : function(f), tli_impl(Triple(f.getParent()->getTargetTriple())),
          tli(tli_impl), assumptions(f), dominators(f),
          basic_aa(f.getParent()->getDataLayout(), f, tli, assumptions,
                   &dominators),
          aliases(tli), facts(f) {
      aliases.addAAResult(basic_aa);
      memory = std::make_unique<MemorySSA>(f, &aliases, &dominators);
    }
  };

  Module &module;
  const DataLayout &layout;
  mutable std::map<const Function *, std::unique_ptr<FunctionState>> states;
  mutable std::set<const LoadInst *> load_active;
  mutable llvm::DenseMap<const LoadInst *, const Value *> load_value_cache;
  mutable llvm::DenseMap<const Value *, Optional<APInt>> integer_cache;
  mutable llvm::SmallPtrSet<const Value *, 32> integer_active;
  mutable llvm::DenseMap<const Value *, Optional<MemoryRegion>> region_cache;
  mutable llvm::SmallPtrSet<const Value *, 32> region_active;
  mutable llvm::DenseMap<const Function *, bool> local_writes_only_cache;
  mutable llvm::DenseMap<std::pair<const Function *, unsigned>,
                         std::vector<unsigned>>
      null_args_cache;
  mutable std::unordered_map<const Value *, std::vector<const Value *>>
      pointer_alternatives_cache;

  struct StringState {
    StringBounds bounds;
    Optional<uint64_t> zero_begin;
    Optional<uint64_t> zero_end;
    const Value *copied_source = nullptr;
    const CallBase *copy_site = nullptr;
  };
  using StringKey = std::pair<const Instruction *, const Value *>;
  mutable std::map<StringKey, StringState> string_cache;
  mutable std::set<StringKey> string_active;

  explicit Impl(Module &m) : module(m), layout(m.getDataLayout()) {}

  FunctionState &state(const Function &function) const {
    auto &entry = states[&function];
    if (!entry)
      entry = std::make_unique<FunctionState>(const_cast<Function &>(function));
    return *entry;
  }

  const Value *loadValue(const LoadInst &load) const {
    const bool top_level = load_active.empty();
    if (top_level) {
      auto cached = load_value_cache.find(&load);
      if (cached != load_value_cache.end())
        return cached->second;
    }
    if (load.isVolatile() || load.isAtomic() || !load_active.insert(&load).second)
      return nullptr;
    auto done = [&](const Value *value) {
      load_active.erase(&load);
      if (top_level)
        load_value_cache[&load] = value;
      return value;
    };
    auto &analysis = state(*load.getFunction());
    MemoryAccess *clobber =
        analysis.memory->getWalker()->getClobberingMemoryAccess(&load);
    for (unsigned depth = 0; depth < 32; ++depth) {
      const auto *definition = dyn_cast<MemoryDef>(clobber);
      const auto *call = definition
                             ? dyn_cast_or_null<CallBase>(definition->getMemoryInst())
                             : nullptr;
      if (!call || !modeledDisjointStackWrite(*call, load.getPointerOperand()))
        break;
      clobber = analysis.memory->getWalker()->getClobberingMemoryAccess(
          definition->getDefiningAccess(), MemoryLocation::get(&load));
    }
    const auto *definition = dyn_cast<MemoryDef>(clobber);
    const auto *store =
        definition ? dyn_cast_or_null<StoreInst>(definition->getMemoryInst())
                   : nullptr;
    if (!store || store->isVolatile() || store->isAtomic() ||
        store->getValueOperand()->getType() != load.getType() ||
        !analysis.dominators.dominates(store, &load) ||
        analysis.aliases.alias(MemoryLocation::get(store),
                               MemoryLocation::get(&load)) !=
            AliasResult::MustAlias)
      return done(nullptr);
    return done(store->getValueOperand());
  }

  bool modeledDisjointStackWrite(const CallBase &call,
                                 const Value *location) const {
    const auto *stack = dyn_cast<AllocaInst>(getUnderlyingObject(location));
    if (!stack)
      return false;
    const Function *target = ValueFacts::callee(call);
    if (!target)
      return false;
    if (LibraryModels::readsOnly(*target) ||
        LibraryModels::allocation(*target) != AllocationKind::Unknown)
      return true;
    const Value *destination = nullptr;
    if (const auto *intrinsic = dyn_cast<MemIntrinsic>(&call))
      destination = intrinsic->getRawDest();
    auto named = [&](const char *name) {
      return ValueFacts::hasLibraryName(*target, name, true, true);
    };
    for (const char *name : {"strcpy", "stpcpy", "wcscpy", "strncpy", "wcsncpy",
                             "strcat", "wcscat", "strncat", "wcsncat", "memcpy",
                             "memmove", "memset", "fgets", "fgetws", "gets", "fread"})
      if (named(name) && call.arg_size())
        destination = call.getArgOperand(0);
    for (const char *name : {"read", "recv", "recvfrom", "readlink"})
      if (named(name) && call.arg_size() > 1)
        destination = call.getArgOperand(1);
    if (named("readlinkat") && call.arg_size() > 2)
      destination = call.getArgOperand(2);
    if (!destination)
      return false;
    auto written = address(destination);
    if (!written || written->object == stack)
      return false;
    if (isa<AllocaInst>(written->object))
      return true;
    if ((isa<Argument>(written->object) || isa<GlobalValue>(written->object)) &&
        !PointerMayBeCaptured(stack, true, true))
      return true;
    if (const auto *allocation = dyn_cast<CallBase>(written->object))
      if (const Function *allocator = ValueFacts::callee(*allocation))
        return LibraryModels::allocation(*allocator) != AllocationKind::Unknown;
    return false;
  }

  const Value *resolve(const Value *value, unsigned depth = 32) const {
    if (!depth)
      return value;
    if (const auto *load = dyn_cast<LoadInst>(value))
      if (const Value *stored = loadValue(*load))
        return resolve(stored, depth - 1);
    return value;
  }

  Optional<APInt> integer(const Value *value, unsigned depth = 32) const {
    if (!depth || !value || !value->getType()->isIntegerTy())
      return None;
    if (const auto *constant = dyn_cast<ConstantInt>(value))
      return constant->getValue();
    const bool top_level = (depth == 32 && integer_active.empty());
    if (top_level) {
      auto cached = integer_cache.find(value);
      if (cached != integer_cache.end())
        return cached->second;
    }
    if (!integer_active.insert(value).second)
      return None;
    auto finish = [&](Optional<APInt> res) {
      integer_active.erase(value);
      if (top_level)
        integer_cache[value] = res;
      return res;
    };
    const Value *resolved = resolve(value, depth);
    if (const auto *constant = dyn_cast<ConstantInt>(resolved))
      return finish(constant->getValue());
    if (const auto *call = dyn_cast<CallBase>(resolved)) {
      const Function *target = ValueFacts::callee(*call);
      if (target && call->arg_size() == 1 && call->getType()->isIntegerTy() &&
          (ValueFacts::hasLibraryName(*target, "strlen", true, true) ||
           ValueFacts::hasLibraryName(*target, "wcslen", true, true)))
        if (auto bytes = literalByteLength(call->getArgOperand(0)))
          return finish(APInt(call->getType()->getIntegerBitWidth(),
                              *bytes / characterBytes(call->getArgOperand(0))));
    }
    if (const auto *cast = dyn_cast<CastInst>(resolved)) {
      if (!cast->getType()->isIntegerTy() ||
          (cast->getOpcode() != Instruction::SExt &&
           cast->getOpcode() != Instruction::ZExt &&
           cast->getOpcode() != Instruction::Trunc))
        return finish(None);
      auto input = integer(cast->getOperand(0), depth - 1);
      if (!input)
        return finish(None);
      unsigned width = cast->getType()->getIntegerBitWidth();
      if (cast->getOpcode() == Instruction::SExt)
        return finish(input->sext(width));
      if (cast->getOpcode() == Instruction::ZExt)
        return finish(input->zext(width));
      if (cast->getOpcode() == Instruction::Trunc)
        return finish(input->trunc(width));
    }
    if (const auto *op = dyn_cast<BinaryOperator>(resolved)) {
      auto a = integer(op->getOperand(0), depth - 1);
      if (!a)
        return finish(None);
      auto b = integer(op->getOperand(1), depth - 1);
      if (!b)
        return finish(None);
      if (op->getOpcode() == Instruction::Add)
        return finish(*a + *b);
      if (op->getOpcode() == Instruction::Sub)
        return finish(*a - *b);
      if (op->getOpcode() == Instruction::Mul)
        return finish(*a * *b);
    }
    return finish(None);
  }

  Optional<uint64_t> unsignedInteger(const Value *value) const {
    auto number = integer(value);
    if (!number || number->getActiveBits() > 64)
      return None;
    return number->getZExtValue();
  }

  struct Address {
    const Value *object = nullptr;
    int64_t offset = 0;
  };

  Optional<Address> address(const Value *value, unsigned depth = 32) const {
    if (!depth || !value->getType()->isPointerTy())
      return None;
    value = resolve(value);
    if (const auto *cast = dyn_cast<BitCastOperator>(value))
      return address(cast->getOperand(0), depth - 1);
    if (const auto *gep = dyn_cast<GEPOperator>(value)) {
      auto base = address(gep->getPointerOperand(), depth - 1);
      APInt delta(layout.getIndexTypeSizeInBits(value->getType()), 0);
      if (!base || !gep->accumulateConstantOffset(layout, delta))
        return None;
      APInt total(128, static_cast<uint64_t>(base->offset), true);
      total += delta.sextOrTrunc(128);
      if (total.getMinSignedBits() > 64)
        return None;
      return Address{base->object, total.getSExtValue()};
    }
    return Address{value, 0};
  }

  Optional<int64_t> displacement(const Value *written,
                                const Value *queried) const {
    auto left = address(written), right = address(queried);
    if (!left || !right || left->object != right->object)
      return None;
    APInt delta(128, static_cast<uint64_t>(left->offset), true);
    delta -= APInt(128, static_cast<uint64_t>(right->offset), true);
    return delta.getMinSignedBits() <= 64
               ? Optional<int64_t>(delta.getSExtValue()) : None;
  }

  uint64_t characterBytes(const Value *pointer) const {
    Type *type = pointer->getType();
    if (!type->isPointerTy() || type->getPointerElementType()->isArrayTy())
      return 1;
    auto bytes = typeBytes(type->getPointerElementType(), layout);
    return bytes && (*bytes == 1 || *bytes == 2 || *bytes == 4) ? *bytes : 1;
  }

  Optional<uint64_t> available(const Value *pointer) const {
    auto memory = region(pointer);
    if (!memory || memory->offset < 0 ||
        static_cast<uint64_t>(memory->offset) > memory->capacity)
      return None;
    return memory->capacity - static_cast<uint64_t>(memory->offset);
  }

  Optional<uint64_t> literalByteLength(const Value *pointer) const {
    pointer = resolve(pointer);
    const auto *global = dyn_cast<GlobalVariable>(getUnderlyingObject(pointer));
    if (!global || !global->isConstant())
      return None;
    uint64_t width = characterBytes(pointer);
    uint64_t length = GetStringLength(pointer, width * 8);
    return length ? product(length - 1, width) : None;
  }

  StringState initialString(const Value *pointer) const {
    StringState result;
    if (auto literal = literalByteLength(pointer)) {
      uint64_t bytes = *literal;
      result.bounds.termination = StringTermination::Terminated;
      result.bounds.minimum_bytes = bytes;
      result.bounds.maximum_bytes = bytes;
      result.bounds.value_flow = true;
      result.zero_begin = bytes;
      result.zero_end = bytes + characterBytes(pointer);
      return result;
    }
    auto memory = region(pointer);
    const auto *allocation = memory
                                 ? dyn_cast<AllocaInst>(memory->allocation)
                                 : nullptr;
    if (allocation &&
        (allocation->getAllocatedType()->isArrayTy() ||
         allocation->getAllocatedType()->isStructTy())) {
      result.bounds.termination = StringTermination::Unproven;
      result.bounds.evidence.push_back(allocation);
    }
    return result;
  }

  bool localWritesOnly(const Function &function) const {
    if (function.isDeclaration())
      return false;
    auto cached = local_writes_only_cache.find(&function);
    if (cached != local_writes_only_cache.end())
      return cached->second;
    bool result = true;
    for (const BasicBlock &block : function) {
      for (const Instruction &inst : block) {
        if (!inst.mayWriteToMemory())
          continue;
        if (const auto *store = dyn_cast<StoreInst>(&inst))
          if (const auto *object = dyn_cast<AllocaInst>(
                  getUnderlyingObject(store->getPointerOperand())))
            if (object->getFunction() == &function)
              continue;
        if (const auto *call = dyn_cast<CallBase>(&inst))
          if (const Function *target = ValueFacts::callee(*call))
            if (LibraryModels::readsOnly(*target) ||
                target->getIntrinsicID() == Intrinsic::lifetime_start ||
                target->getIntrinsicID() == Intrinsic::lifetime_end)
              continue;
        result = false;
        break;
      }
      if (!result)
        break;
    }
    local_writes_only_cache[&function] = result;
    return result;
  }

  static Optional<uint64_t> sum(Optional<uint64_t> a,
                                Optional<uint64_t> b) {
    if (!a || !b || *b > std::numeric_limits<uint64_t>::max() - *a)
      return None;
    return *a + *b;
  }

  void collectPointerAlternatives(const Value *pointer, unsigned depth,
                                  SmallPtrSetImpl<const Value *> &visited,
                                  std::vector<const Value *> &out,
                                  bool &overflowed) const {
    if (overflowed || !pointer)
      return;
    if (!depth) {
      out.push_back(pointer);
      if (out.size() > 32)
        overflowed = true;
      return;
    }
    if (!visited.insert(pointer).second)
      return;
    const Value *resolved = resolve(pointer);
    if (resolved != pointer) {
      collectPointerAlternatives(resolved, depth - 1, visited, out, overflowed);
      return;
    }
    SmallVector<const Value *, 8> inputs;
    if (const auto *phi = dyn_cast<PHINode>(pointer))
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
        inputs.push_back(phi->getIncomingValue(i));
    else if (const auto *select = dyn_cast<SelectInst>(pointer)) {
      inputs.push_back(select->getTrueValue());
      inputs.push_back(select->getFalseValue());
    } else if (const auto *load = dyn_cast<LoadInst>(pointer)) {
      const auto *slot =
          dyn_cast<AllocaInst>(getUnderlyingObject(load->getPointerOperand()));
      if (!slot || load->isAtomic() || load->isVolatile()) {
        out.push_back(pointer);
        if (out.size() > 32)
          overflowed = true;
        return;
      }
      auto &analysis = state(*load->getFunction());
      SmallPtrSet<MemoryAccess *, 32> seen;
      std::function<void(MemoryAccess *, unsigned)> collect;
      collect = [&](MemoryAccess *memory, unsigned remaining) {
        if (inputs.size() > 32) {
          overflowed = true;
          return;
        }
        if (!remaining || !memory) {
          inputs.push_back(pointer);
          return;
        }
        memory = analysis.memory->getWalker()->getClobberingMemoryAccess(
            memory, MemoryLocation::get(load));
        if (!seen.insert(memory).second) {
          inputs.push_back(pointer);
          return;
        }
        if (const auto *phi = dyn_cast<MemoryPhi>(memory)) {
          for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
            collect(phi->getIncomingValue(i), remaining - 1);
          return;
        }
        const auto *definition = dyn_cast<MemoryDef>(memory);
        const auto *store =
            definition ? dyn_cast_or_null<StoreInst>(definition->getMemoryInst())
                       : nullptr;
        if (store && analysis.aliases.alias(MemoryLocation::get(store),
                                            MemoryLocation::get(load)) ==
                         AliasResult::MustAlias) {
          inputs.push_back(store->getValueOperand());
          return;
        }
        const auto *call =
            definition ? dyn_cast_or_null<CallBase>(definition->getMemoryInst())
                       : nullptr;
        if (call && modeledDisjointStackWrite(*call, load->getPointerOperand()))
          collect(definition->getDefiningAccess(), remaining - 1);
        else
          inputs.push_back(pointer);
      };
      collect(analysis.memory->getWalker()->getClobberingMemoryAccess(load),
              depth);
    }
    if (overflowed)
      return;
    if (inputs.empty()) {
      out.push_back(pointer);
      if (out.size() > 32)
        overflowed = true;
      return;
    }
    for (const Value *input : inputs) {
      if (overflowed)
        return;
      if (input == pointer) {
        out.push_back(pointer);
        if (out.size() > 32)
          overflowed = true;
        continue;
      }
      collectPointerAlternatives(input, depth - 1, visited, out, overflowed);
    }
  }

  std::vector<const Value *> pointerAlternatives(const Value *pointer) const {
    auto cached = pointer_alternatives_cache.find(pointer);
    if (cached != pointer_alternatives_cache.end())
      return cached->second;
    SmallPtrSet<const Value *, 32> visited;
    std::vector<const Value *> result;
    bool overflowed = false;
    collectPointerAlternatives(pointer, 24, visited, result, overflowed);
    if (overflowed || result.empty())
      result = {pointer};
    pointer_alternatives_cache.emplace(pointer, result);
    return result;
  }

  static void appendUniqueEvidence(std::vector<const Instruction *> &dst,
                                   const std::vector<const Instruction *> &src) {
    for (const Instruction *inst : src) {
      if (std::find(dst.begin(), dst.end(), inst) == dst.end())
        dst.push_back(inst);
    }
  }

  static StringState mergeStrings(const std::vector<StringState> &inputs) {
    if (inputs.empty())
      return {};
    StringState merged = inputs.front();
    for (size_t i = 1; i < inputs.size(); ++i) {
      const auto &next = inputs[i];
      if (merged.bounds.termination != next.bounds.termination)
        merged.bounds.termination = StringTermination::Unknown;
      if (!merged.bounds.minimum_bytes || !next.bounds.minimum_bytes)
        merged.bounds.minimum_bytes = None;
      else
        merged.bounds.minimum_bytes = std::min(*merged.bounds.minimum_bytes,
                                               *next.bounds.minimum_bytes);
      if (!merged.bounds.maximum_bytes || !next.bounds.maximum_bytes)
        merged.bounds.maximum_bytes = None;
      else
        merged.bounds.maximum_bytes = std::max(*merged.bounds.maximum_bytes,
                                               *next.bounds.maximum_bytes);
      if (!merged.zero_begin || !merged.zero_end || !next.zero_begin || !next.zero_end) {
        merged.zero_begin = merged.zero_end = None;
      } else {
        merged.zero_begin = std::max(*merged.zero_begin, *next.zero_begin);
        merged.zero_end = std::min(*merged.zero_end, *next.zero_end);
        if (*merged.zero_begin >= *merged.zero_end)
          merged.zero_begin = merged.zero_end = None;
      }
      merged.bounds.value_flow |= next.bounds.value_flow;
      if (merged.copied_source != next.copied_source)
        merged.copied_source = nullptr;
      appendUniqueEvidence(merged.bounds.evidence, next.bounds.evidence);
    }
    return merged;
  }

  struct StringFromMemoEntry {
    StringState state;
    uint64_t dep_mask = 0;
  };

  struct StringFromContext {
    llvm::DenseMap<MemoryAccess *, StringFromMemoEntry> exact;
    llvm::DenseMap<std::pair<MemoryAccess *, unsigned>, StringFromMemoEntry>
        by_depth;
    llvm::DenseMap<MemoryAccess *, MemoryAccess *> clobber;
    llvm::DenseMap<MemoryAccess *, unsigned> active_pos;
    std::vector<llvm::SmallVector<MemoryAccess *, 8>> undo_exact;
    std::vector<llvm::SmallVector<std::pair<MemoryAccess *, unsigned>, 8>>
        undo_depth;
  };

  StringState stringState(const Value *pointer, const Instruction &at) const {
    pointer = resolve(pointer);
    StringKey key{&at, pointer};
    auto cached = string_cache.find(key);
    if (cached != string_cache.end())
      return cached->second;
    if (!string_active.insert(key).second)
      return {};
    StringState result = initialString(pointer);
    auto alternatives = pointerAlternatives(pointer);
    if (alternatives.size() > 1 ||
        (alternatives.size() == 1 && alternatives[0] != pointer)) {
      std::vector<StringState> inputs;
      for (const Value *input : alternatives)
        inputs.push_back(input == pointer ? StringState{} : stringState(input, at));
      result = mergeStrings(inputs);
      string_active.erase(key);
      string_cache[key] = result;
      return result;
    }
    if (literalByteLength(pointer)) {
      string_active.erase(key);
      string_cache[key] = result;
      return result;
    }
    auto &analysis = state(*at.getFunction());
    auto *access = dyn_cast_or_null<MemoryUseOrDef>(
        analysis.memory->getMemoryAccess(&at));
    if (access) {
      StringFromContext ctx;
      uint64_t dep_mask = 0;
      bool hit_zero = false;
      result = stringFrom(access->getDefiningAccess(), pointer, at, 64, ctx,
                          dep_mask, hit_zero);
    }
    string_active.erase(key);
    string_cache[key] = result;
    return result;
  }

  StringState stringFrom(MemoryAccess *memory, const Value *pointer,
                         const Instruction &at, unsigned depth,
                         StringFromContext &ctx, uint64_t &dep_mask,
                         bool &hit_zero) const {
    auto &analysis = state(*at.getFunction());
    if (!depth || !memory) {
      if (!depth)
        hit_zero = true;
      return {};
    }
    if (analysis.memory->isLiveOnEntryDef(memory))
      return initialString(pointer);
    auto capacity = available(pointer);
    MemoryLocation location(pointer, capacity ? LocationSize::precise(*capacity)
                                             : LocationSize::beforeOrAfterPointer());
    auto clob_it = ctx.clobber.find(memory);
    if (clob_it != ctx.clobber.end()) {
      memory = clob_it->second;
    } else {
      MemoryAccess *resolved_clob =
          analysis.memory->getWalker()->getClobberingMemoryAccess(memory,
                                                                  location);
      ctx.clobber[memory] = resolved_clob;
      memory = resolved_clob;
    }
    if (analysis.memory->isLiveOnEntryDef(memory))
      return initialString(pointer);
    auto exact_it = ctx.exact.find(memory);
    if (exact_it != ctx.exact.end()) {
      dep_mask |= exact_it->second.dep_mask;
      return exact_it->second.state;
    }
    auto depth_it = ctx.by_depth.find({memory, depth});
    if (depth_it != ctx.by_depth.end()) {
      dep_mask |= depth_it->second.dep_mask;
      hit_zero = true;
      return depth_it->second.state;
    }
    auto act_it = ctx.active_pos.find(memory);
    if (act_it != ctx.active_pos.end()) {
      if (act_it->second < 64)
        dep_mask |= (uint64_t(1) << act_it->second);
      return {};
    }
    const unsigned my_pos = static_cast<unsigned>(ctx.active_pos.size());
    ctx.active_pos.try_emplace(memory, my_pos);
    if (ctx.undo_exact.size() <= my_pos) {
      ctx.undo_exact.resize(my_pos + 1);
      ctx.undo_depth.resize(my_pos + 1);
    }
    uint64_t local_mask = 0;
    bool local_hit_zero = false;
    auto done = [&](StringState result) {
      ctx.active_pos.erase(memory);
      for (MemoryAccess *stale : ctx.undo_exact[my_pos])
        ctx.exact.erase(stale);
      ctx.undo_exact[my_pos].clear();
      for (const auto &stale : ctx.undo_depth[my_pos])
        ctx.by_depth.erase(stale);
      ctx.undo_depth[my_pos].clear();

      // Any cycle back to `memory` itself is self-contained; only active
      // ancestors strictly above `my_pos` constrain memo lifetime.
      uint64_t outer_mask =
          my_pos < 64 ? (local_mask & ~(uint64_t(1) << my_pos)) : local_mask;
      dep_mask |= outer_mask;
      if (local_hit_zero)
        hit_zero = true;
      if (my_pos < 64) {
        if (!local_hit_zero) {
          ctx.exact[memory] = {result, outer_mask};
          if (outer_mask != 0) {
            unsigned deepest = 63u - static_cast<unsigned>(__builtin_clzll(outer_mask));
            ctx.undo_exact[deepest].push_back(memory);
          }
        } else {
          ctx.by_depth[{memory, depth}] = {result, outer_mask};
          if (outer_mask != 0) {
            unsigned deepest = 63u - static_cast<unsigned>(__builtin_clzll(outer_mask));
            ctx.undo_depth[deepest].push_back({memory, depth});
          }
        }
      }
      return result;
    };
    if (auto *phi = dyn_cast<MemoryPhi>(memory)) {
      StringState merged;
      bool first = true;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        auto next = stringFrom(phi->getIncomingValue(i), pointer, at,
                               depth - 1, ctx, local_mask, local_hit_zero);
        if (first) {
          merged = next;
          first = false;
        } else {
          if (merged.bounds.termination != next.bounds.termination)
            merged.bounds.termination = StringTermination::Unknown;
          if (!merged.bounds.minimum_bytes || !next.bounds.minimum_bytes)
            merged.bounds.minimum_bytes = None;
          else
            merged.bounds.minimum_bytes = std::min(*merged.bounds.minimum_bytes,
                                                   *next.bounds.minimum_bytes);
          if (!merged.bounds.maximum_bytes || !next.bounds.maximum_bytes)
            merged.bounds.maximum_bytes = None;
          else
            merged.bounds.maximum_bytes = std::max(*merged.bounds.maximum_bytes,
                                                   *next.bounds.maximum_bytes);
          if (!merged.zero_begin || !merged.zero_end || !next.zero_begin ||
              !next.zero_end) {
            merged.zero_begin = merged.zero_end = None;
          } else {
            merged.zero_begin = std::max(*merged.zero_begin, *next.zero_begin);
            merged.zero_end = std::min(*merged.zero_end, *next.zero_end);
            if (*merged.zero_begin >= *merged.zero_end)
              merged.zero_begin = merged.zero_end = None;
          }
          merged.bounds.value_flow |= next.bounds.value_flow;
          if (merged.copied_source != next.copied_source)
            merged.copied_source = nullptr;
          appendUniqueEvidence(merged.bounds.evidence, next.bounds.evidence);
        }
      }
      return done(merged);
    }
    auto *definition = dyn_cast<MemoryDef>(memory);
    if (!definition)
      return done({});
    const Instruction *writer = definition->getMemoryInst();
    auto previous = [&]() {
      return stringFrom(definition->getDefiningAccess(), pointer, at,
                         depth - 1, ctx, local_mask, local_hit_zero);
    };
    auto disjointDestination = [&](const Value *destination) {
      return analysis.aliases.alias(
                 MemoryLocation(destination, LocationSize::beforeOrAfterPointer()),
                 location) == AliasResult::NoAlias;
    };
    auto terminated = [&](Optional<uint64_t> minimum,
                          Optional<uint64_t> maximum,
                          uint64_t width) {
      StringState result;
      result.bounds.termination = StringTermination::Terminated;
      result.bounds.minimum_bytes = minimum;
      result.bounds.maximum_bytes = maximum;
      result.bounds.value_flow = bool(maximum);
      result.bounds.evidence.push_back(writer);
      if (minimum && maximum && *minimum == *maximum) {
        result.zero_begin = *maximum;
        result.zero_end = sum(maximum, width);
      }
      return result;
    };
    auto overwrite = [&](Optional<uint64_t> bytes) {
      auto result = previous();
      if (!bytes)
        return StringState{};
      if (!*bytes)
        return result;
      if (result.zero_begin && result.zero_end && *result.zero_end > *bytes) {
        result.zero_begin = std::max(*result.zero_begin, *bytes);
        result.bounds.minimum_bytes = 0;
        result.bounds.maximum_bytes = *result.zero_begin;
        result.bounds.termination = StringTermination::Terminated;
      } else {
        result.bounds = {};
        result.zero_begin = result.zero_end = None;
        if (capacity && *bytes >= *capacity) {
          result.bounds.termination = StringTermination::Unproven;
          result.bounds.evidence.push_back(writer);
        }
      }
      result.copied_source = nullptr;
      result.copy_site = nullptr;
      return result;
    };
    if (const auto *store = dyn_cast<StoreInst>(writer)) {
      auto delta = displacement(store->getPointerOperand(), pointer);
      auto value = integer(store->getValueOperand());
      auto width = typeBytes(store->getValueOperand()->getType(), layout, false);
      if (delta && *delta >= 0 && width && value && value->isZero() &&
          (!capacity || static_cast<uint64_t>(*delta) + *width <= *capacity)) {
        auto result = terminated(0, static_cast<uint64_t>(*delta), *width);
        result.zero_begin = static_cast<uint64_t>(*delta);
        result.zero_end = static_cast<uint64_t>(*delta) + *width;
        return done(result);
      }
      if (!delta || !width)
        return done({});
      auto result = previous();
      if (*delta >= 0 && result.zero_begin && result.zero_end &&
          static_cast<uint64_t>(*delta) < *result.zero_end &&
          static_cast<uint64_t>(*delta) + *width > *result.zero_begin)
        return done({});
      if (result.bounds.maximum_bytes && *delta >= 0 &&
          static_cast<uint64_t>(*delta) <= *result.bounds.maximum_bytes)
        result.bounds.minimum_bytes = 0;
      return done(result);
    }
    const auto *call = dyn_cast<CallBase>(writer);
    const Function *target = call ? ValueFacts::callee(*call) : nullptr;
    if (!target)
      return done({});
    // Whole-function AA can conservatively regard a local array as escaped
    // because a later call captures it. That does not let an earlier unrelated
    // call alter this array. Capture-before is a temporal memory-effect fact.
    if (auto object = address(pointer))
      if (const auto *allocation = dyn_cast<AllocaInst>(object->object)) {
        bool passed = false;
        for (const Use &argument : call->args()) {
          if (!argument->getType()->isPointerTy())
            continue;
          const Value *origin = argument.get();
          for (unsigned i = 0; i < 8; ++i) {
            const Value *next = getUnderlyingObject(resolve(origin));
            if (next == allocation) {
              passed = true;
              break;
            }
            if (next == origin)
              break;
            origin = next;
          }
        }
        if (!passed && !PointerMayBeCapturedBefore(allocation, true, true, call,
                                                     &analysis.dominators, true))
          return done(previous());
      }
    auto named = [&](const char *name) {
      return ValueFacts::hasLibraryName(*target, name, true, true);
    };
    if (target->getIntrinsicID() == Intrinsic::lifetime_start ||
        target->getIntrinsicID() == Intrinsic::lifetime_end ||
        LibraryModels::readsOnly(*target) || localWritesOnly(*target) ||
        named("strdup") || named("strndup"))
      return done(previous());
    const Value *destination = nullptr;
    const Value *source = nullptr;
    Optional<uint64_t> count;
    bool set = false;
    if (const auto *intrinsic = dyn_cast<MemIntrinsic>(call)) {
      destination = intrinsic->getRawDest();
      count = unsignedInteger(intrinsic->getLength());
      if (const auto *transfer = dyn_cast<MemTransferInst>(intrinsic))
        source = transfer->getRawSource();
      else
        set = true;
    } else if ((named("memcpy") || named("memmove") || named("memset")) &&
               call->arg_size() >= 3) {
      destination = call->getArgOperand(0);
      count = unsignedInteger(call->getArgOperand(2));
      set = named("memset");
      if (!set)
        source = call->getArgOperand(1);
    }
    if (destination) {
      auto delta = displacement(destination, pointer);
      if (!delta || *delta != 0)
        return done(disjointDestination(destination) ? previous() : StringState{});
      if (set) {
        const Value *value = isa<MemSetInst>(call)
                                 ? cast<MemSetInst>(call)->getValue()
                                 : call->getArgOperand(1);
        auto byte = integer(value);
        if (count && *count && byte && byte->zextOrTrunc(8).isZero()) {
          auto result = terminated(0, 0, characterBytes(pointer));
          result.zero_begin = 0;
          result.zero_end = count;
          return done(result);
        }
        return done(overwrite(count));
      }
      auto input = stringState(source, *call);
      uint64_t width = characterBytes(source);
      if (count && input.bounds.maximum_bytes &&
          *count >= *input.bounds.maximum_bytes + width) {
        input.bounds.evidence.push_back(call);
        return done(input);
      }
      if (count && input.bounds.minimum_bytes &&
          *count <= *input.bounds.minimum_bytes &&
          capacity && *count >= *capacity) {
        StringState result;
        result.bounds.termination = StringTermination::Unproven;
        result.bounds.evidence.push_back(call);
        return done(result);
      }
      return done(overwrite(count));
    }
    if ((named("strcpy") || named("stpcpy") || named("wcscpy") ||
         named("strncpy") || named("wcsncpy") || named("strcat") ||
         named("wcscat") || named("strncat") || named("wcsncat")) &&
        call->arg_size() >= 2) {
      auto delta = displacement(call->getArgOperand(0), pointer);
      if (!delta || *delta != 0)
        return done(disjointDestination(call->getArgOperand(0))
                        ? previous() : StringState{});
      auto input = stringState(call->getArgOperand(1), *call);
      uint64_t width = characterBytes(call->getArgOperand(0));
      bool bounded = named("strncpy") || named("wcsncpy") ||
                     named("strncat") || named("wcsncat");
      bool append = named("strcat") || named("wcscat") ||
                    named("strncat") || named("wcsncat");
      Optional<uint64_t> limit;
      if (bounded && call->arg_size() >= 3)
        if (auto characters = unsignedInteger(call->getArgOperand(2)))
          limit = product(*characters, width);
      if (bounded && !append) {
        if (!limit || !input.bounds.maximum_bytes ||
            *input.bounds.maximum_bytes >= *limit)
          return done(overwrite(limit));
      }
      auto minimum = input.bounds.minimum_bytes;
      auto maximum = input.bounds.maximum_bytes;
      if (bounded && append) {
        if (limit) {
          minimum = minimum ? std::min(*minimum, *limit) : Optional<uint64_t>(0);
          maximum = maximum ? std::min(*maximum, *limit) : limit;
        } else
          maximum = None;
      }
      if (append) {
        auto old = previous();
        if (old.bounds.termination != StringTermination::Terminated)
          return done({});
        minimum = sum(old.bounds.minimum_bytes, minimum);
        maximum = sum(old.bounds.maximum_bytes, maximum);
      }
      auto result = terminated(minimum, maximum, width);
      result.bounds.value_flow = bool(maximum);
      result.copied_source = append ? nullptr : resolve(call->getArgOperand(1));
      result.copy_site = call;
      return done(result);
    }
    if ((named("readlink") && call->arg_size() >= 3) ||
        (named("readlinkat") && call->arg_size() >= 4)) {
      unsigned destinationIndex = named("readlink") ? 1 : 2;
      auto delta = displacement(call->getArgOperand(destinationIndex), pointer);
      if (!delta || *delta != 0)
        return done(disjointDestination(call->getArgOperand(destinationIndex))
                        ? previous() : StringState{});
      return done(overwrite(unsignedInteger(call->getArgOperand(destinationIndex + 1))));
    }
    if (((named("read") || named("recv") || named("recvfrom")) && call->arg_size() >= 3) ||
        (named("fread") && call->arg_size() >= 3)) {
      unsigned destinationIndex = named("fread") ? 0 : 1;
      auto delta = displacement(call->getArgOperand(destinationIndex), pointer);
      if (!delta || *delta != 0)
        return done(disjointDestination(call->getArgOperand(destinationIndex))
                        ? previous() : StringState{});
      Optional<uint64_t> bytes;
      if (named("fread")) {
        auto elements = unsignedInteger(call->getArgOperand(2));
        auto width = unsignedInteger(call->getArgOperand(1));
        if (elements && width)
          bytes = product(*elements, *width);
      } else
        bytes = unsignedInteger(call->getArgOperand(2));
      return done(overwrite(bytes));
    }
    if ((named("sprintf") || named("snprintf") || named("swprintf")) &&
        call->arg_size() >= 2) {
      auto delta = displacement(call->getArgOperand(0), pointer);
      if (!delta || *delta != 0)
        return done({});
      auto output = formatBounds(*call);
      uint64_t width = characterBytes(call->getArgOperand(0));
      auto maximum = output.maximum_bytes;
      if (maximum && *maximum >= width)
        maximum = *maximum - width;
      if (!named("sprintf") && call->arg_size() >= 3) {
        auto limit = unsignedInteger(call->getArgOperand(1));
        if (!limit || !*limit)
          return done(limit ? previous() : StringState{});
        if (named("swprintf"))
          limit = product(*limit, width);
        if (limit && *limit >= width)
          maximum = maximum ? std::min(*maximum, *limit - width)
                            : Optional<uint64_t>(*limit - width);
      }
      return done(terminated(0, maximum, width));
    }
    return done({});
  }

  struct PrintConversion {
    char conversion = 0;
    unsigned argument = 0;
    uint64_t width = 0;
    Optional<uint64_t> precision;
    std::string length;
    bool alternate = false;
    bool signed_prefix = false;
  };
  struct PrintFormat {
    uint64_t literals = 0;
    std::vector<PrintConversion> conversions;
  };

  Optional<PrintFormat> printFormat(const CallBase &call) const {
    const Function *target = ValueFacts::callee(call);
    auto model = target ? LibraryModels::format(*target) : None;
    if (!model || model->scanf || model->format >= call.arg_size())
      return None;
    auto text = ValueFacts::constantString(*resolve(call.getArgOperand(model->format)));
    if (!text || !parseFormat(*text, false))
      return None;
    PrintFormat result;
    unsigned next = model->first_argument;
    auto number = [&](size_t &position) -> Optional<uint64_t> {
      uint64_t value = 0;
      bool any = false;
      while (position < text->size() && std::isdigit((*text)[position])) {
        unsigned digit = (*text)[position++] - '0';
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
          return None;
        value = value * 10 + digit;
        any = true;
      }
      return any ? Optional<uint64_t>(value) : None;
    };
    auto positional = [&](size_t &position) -> Optional<unsigned> {
      size_t start = position;
      auto n = number(position);
      if (!n || !*n || *n > std::numeric_limits<unsigned>::max() ||
          position >= text->size() || (*text)[position] != '$') {
        position = start;
        return None;
      }
      ++position;
      return model->first_argument + static_cast<unsigned>(*n) - 1;
    };
    auto star = [&](size_t &position) -> Optional<APInt> {
      ++position;
      auto index = positional(position);
      unsigned argument = index ? *index : next++;
      return argument < call.arg_size() ? integer(call.getArgOperand(argument)) : None;
    };
    for (size_t i = 0; i < text->size();) {
      if ((*text)[i++] != '%') {
        ++result.literals;
        continue;
      }
      if (i < text->size() && (*text)[i] == '%') {
        ++result.literals;
        ++i;
        continue;
      }
      auto index = positional(i);
      PrintConversion conversion;
      while (i < text->size() && StringRef("-+ #0'").contains((*text)[i])) {
        if ((*text)[i] == '#')
          conversion.alternate = true;
        if ((*text)[i] == '+' || (*text)[i] == ' ')
          conversion.signed_prefix = true;
        if ((*text)[i] == '\'')
          return None; // Locale-dependent digit grouping.
        ++i;
      }
      if (i < text->size() && (*text)[i] == '*') {
        auto width = star(i);
        if (!width || width->getMinSignedBits() > 64)
          return None;
        APInt absolute = width->isNegative() ? -width->sext(width->getBitWidth() + 1)
                                             : width->zext(width->getBitWidth() + 1);
        if (absolute.getActiveBits() > 64)
          return None;
        conversion.width = absolute.getZExtValue();
      } else if (i < text->size() && std::isdigit((*text)[i])) {
        auto width = number(i);
        if (!width)
          return None;
        conversion.width = *width;
      }
      if (i < text->size() && (*text)[i] == '.') {
        ++i;
        if (i < text->size() && (*text)[i] == '*') {
          auto precision = star(i);
          if (!precision || precision->getMinSignedBits() > 64)
            return None;
          if (!precision->isNegative())
            conversion.precision = precision->getZExtValue();
        } else {
          conversion.precision = 0;
          if (i < text->size() && std::isdigit((*text)[i])) {
            auto precision = number(i);
            if (!precision)
              return None;
            conversion.precision = *precision;
          }
        }
      }
      while (i < text->size() && StringRef("hljztL").contains((*text)[i]))
        conversion.length += (*text)[i++];
      if (i >= text->size())
        return None;
      conversion.conversion = (*text)[i++];
      if (conversion.conversion == 'm')
        return None; // Locale-dependent strerror output.
      conversion.argument = index ? *index : next++;
      if (conversion.argument >= call.arg_size())
        return None;
      result.conversions.push_back(conversion);
    }
    return result;
  }

  ConstantRange integerRange(const Value *value, const Instruction &at,
                              bool signedValue) const {
    value = resolve(value);
    auto &analysis = state(*at.getFunction());
    ConstantRange range = computeConstantRange(value, signedValue, true,
                                               &analysis.assumptions, &at,
                                               &analysis.dominators);
    if (const auto *cast = dyn_cast<CastInst>(value)) {
      if (cast->getType()->isIntegerTy() &&
          cast->getOperand(0)->getType()->isIntegerTy()) {
        unsigned width = cast->getType()->getIntegerBitWidth();
        if (cast->getOpcode() == Instruction::SExt)
          range = range.intersectWith(
              integerRange(cast->getOperand(0), at, true).signExtend(width));
        if (cast->getOpcode() == Instruction::ZExt)
          range = range.intersectWith(
              integerRange(cast->getOperand(0), at, false).zeroExtend(width));
      }
    }
    if (const auto *call = dyn_cast<CallBase>(value)) {
      const Function *target = ValueFacts::callee(*call);
      Triple triple(module.getTargetTriple());
      bool standardEOF = triple.isOSWindows() || triple.isOSLinux() || triple.isOSDarwin();
      if (standardEOF && target && target->isDeclaration() &&
          value->getType()->isIntegerTy() &&
          value->getType()->getIntegerBitWidth() >= 16)
        for (const char *name : {"getc", "fgetc", "getchar"})
          if (ValueFacts::hasLibraryName(*target, name, true, true)) {
            unsigned width = value->getType()->getIntegerBitWidth();
            range = range.intersectWith(ConstantRange(APInt::getAllOnes(width), APInt(width, 256)));
          }
    }
    auto *instruction = dyn_cast<Instruction>(value);
    if (!instruction || instruction->getFunction() == at.getFunction()) {
      ScalarEvolution &evo = analysis.getEvolution();
      auto *scev = evo.getSCEV(const_cast<Value *>(value));
      range = range.intersectWith(signedValue ? evo.getSignedRange(scev)
                                              : evo.getUnsignedRange(scev));
    }
    if (const auto *domNode = analysis.dominators.getNode(
            const_cast<BasicBlock *>(at.getParent()))) {
      for (const auto *cur = domNode->getIDom(); cur; cur = cur->getIDom()) {
        const BasicBlock &block = *cur->getBlock();
        const auto *branch = dyn_cast<BranchInst>(block.getTerminator());
        if (!branch || !branch->isConditional())
          continue;
        bool yes = analysis.dominators.dominates(
            BasicBlockEdge(branch->getParent(), branch->getSuccessor(0)),
            at.getParent());
        bool no = analysis.dominators.dominates(
            BasicBlockEdge(branch->getParent(), branch->getSuccessor(1)),
            at.getParent());
        if (yes == no)
          continue;
        const auto *cmp = dyn_cast<ICmpInst>(branch->getCondition());
        if (!cmp)
          continue;
        auto predicate = yes ? cmp->getPredicate() : cmp->getInversePredicate();
        const Value *other = nullptr;
        if (analysis.facts.equivalent(*value, *resolve(cmp->getOperand(0))))
          other = cmp->getOperand(1);
        else if (analysis.facts.equivalent(*value,
                                           *resolve(cmp->getOperand(1)))) {
          other = cmp->getOperand(0);
          predicate = ICmpInst::getSwappedPredicate(predicate);
        }
        auto bound = other ? integer(other) : None;
        if (bound &&
            bound->getBitWidth() == value->getType()->getIntegerBitWidth())
          range = range.intersectWith(ConstantRange::makeAllowedICmpRegion(
              predicate, ConstantRange(*bound)));
      }
    }
    return range;
  }

  static uint64_t digits(const APInt &number, unsigned radix, bool signedValue) {
    SmallString<80> text;
    number.toString(text, radix, signedValue);
    return text.size();
  }

  FormatBounds formatBounds(const CallBase &call) const {
    FormatBounds result;
    const Function *callee = ValueFacts::callee(call);
    if (callee) {
      for (const char *name : {"sprintf", "vsprintf", "wsprintf", "snprintf",
                               "vsnprintf", "snprintf_s", "swprintf", "vswprintf"})
        if (ValueFacts::hasLibraryName(*callee, name, true, true))
          result.output_buffer = true;
      for (const char *name : {"snprintf", "vsnprintf", "snprintf_s", "swprintf", "vswprintf"})
        if (ValueFacts::hasLibraryName(*callee, name, true, true))
          result.explicit_limit_argument = 1;
    }
    auto format = printFormat(call);
    if (!format)
      return result;
    uint64_t maximum = format->literals, limited = maximum;
    result.value_flow = format->conversions.empty();
    bool complete = true;
    for (const PrintConversion &conversion : format->conversions) {
      const Value *argument = call.getArgOperand(conversion.argument);
      Optional<uint64_t> size;
      char c = std::tolower(conversion.conversion);
      uint64_t reduced = 0;
      if (c == 's') {
        auto input = stringState(argument, call);
        auto length = input.bounds.maximum_bytes;
        bool wide = conversion.length == "l";
        if (wide) {
          // Without a locale model, only ASCII wide literals have a known
          // multibyte converted length for narrow printf output.
          auto literal = ValueFacts::constantString(*resolve(argument));
          length = literal ? Optional<uint64_t>(literal->size()) : None;
        }
        if (conversion.precision)
          size = length ? std::min(*length, *conversion.precision)
                        : conversion.precision;
        else {
          size = length;
          result.unbounded_string_arguments.push_back(conversion.argument);
        }
        result.value_flow |= bool(size);
      } else if (c == 'c') {
        if (!conversion.length.empty())
          size = None;
        else
          size = 1;
      } else if (c == 'n') {
        size = 0;
      } else if (StringRef("diuox").contains(c) && argument->getType()->isIntegerTy()) {
        bool signedValue = c == 'd' || c == 'i';
        unsigned width = argument->getType()->getIntegerBitWidth();
        unsigned displayWidth = 32;
        if (callee && callee->getReturnType()->isIntegerTy()) {
          unsigned intWidth = callee->getReturnType()->getIntegerBitWidth();
          if (intWidth == 16 || intWidth == 32)
            displayWidth = intWidth;
        }
        if (conversion.length == "hh")
          displayWidth = 8;
        else if (conversion.length == "h")
          displayWidth = 16;
        else if (conversion.length == "ll" || conversion.length == "j")
          displayWidth = 64;
        else if (conversion.length == "l") {
          auto bytes = ValueFacts::macroInteger(module, "__SIZEOF_LONG__");
          displayWidth = bytes && *bytes > 0 && *bytes <= 16 ? *bytes * 8
                         : Triple(module.getTargetTriple()).isOSWindows()
                               ? 32 : layout.getPointerSizeInBits();
        } else if (conversion.length == "z" || conversion.length == "t")
          displayWidth = layout.getPointerSizeInBits();
        width = std::min(width, displayWidth);
        unsigned radix = c == 'x' ? 16 : c == 'o' ? 8 : 10;
        ConstantRange range = integerRange(argument, call, signedValue);
        if (width < range.getBitWidth())
          range = range.truncate(width);
        APInt low = signedValue ? range.getSignedMin() : range.getUnsignedMin();
        APInt high = signedValue ? range.getSignedMax() : range.getUnsignedMax();
        size = std::max(digits(low, radix, signedValue), digits(high, radix, signedValue));
        uint64_t rawDigits = *size;
        bool onlyZero = low.isZero() && high.isZero();
        if (onlyZero && conversion.precision && !*conversion.precision)
          size = 0; // Integer precision zero suppresses a zero value.
        result.value_flow |= !range.isFullSet();
        if (conversion.precision) {
          auto required = sum(conversion.precision,
                              signedValue && low.isNegative() ? 1u : 0u);
          size = required ? Optional<uint64_t>(std::max(*size, *required)) : None;
        }
        if (conversion.alternate && c == 'x' && !onlyZero)
          size = sum(size, 2);
        if (conversion.alternate && c == 'o') {
          if (onlyZero)
            size = size ? std::max(*size, uint64_t(1)) : size;
          else if (!conversion.precision || *conversion.precision <= rawDigits)
            size = sum(size, 1);
        }
        if (signedValue && conversion.signed_prefix && !low.isNegative())
          size = sum(size, 1);
      } else if (StringRef("feg").contains(c) && argument->getType()->isFloatingPointTy()) {
        // The same double-format worst-case bounds used by CodeQL's Printf
        // library. Long double is deliberately unknown without a target ABI.
        if (conversion.length == "L")
          size = None;
        else {
          uint64_t precision = conversion.precision ? *conversion.precision : 6;
          uint64_t point = precision || conversion.alternate ? 1 : 0;
          Type *sourceType = argument->getType();
          const Value *sourceValue = resolve(argument);
          while (const auto *extension = dyn_cast<FPExtInst>(sourceValue)) {
            sourceValue = resolve(extension->getOperand(0));
            sourceType = sourceValue->getType();
          }
          const auto &semantics = sourceType->getFltSemantics();
          uint64_t integralDigits = 1 + static_cast<uint64_t>(std::floor(
              (APFloat::semanticsMaxExponent(semantics) + 1) * std::log10(2.0)));
          if (c == 'f') {
            size = sum(1 + integralDigits + point, precision);
            result.floating_conversion = true;
          } else if (c == 'e')
            size = sum(7 + point, precision);
          else {
            auto scientific = sum(7 + point, precision);
            auto fixed = sum(std::max(precision, uint64_t(1)), 6);
            if (scientific && fixed)
              size = std::max(*scientific, *fixed);
          }
        }
      } else if (c == 'p' && argument->getType()->isPointerTy()) {
        uint64_t prefix = Triple(module.getTargetTriple()).isOSWindows()
                              ? (conversion.alternate ? 2 : 0) : 2;
        size = std::max<uint64_t>(5, prefix +
                    layout.getPointerTypeSizeInBits(argument->getType()) / 4);
        result.value_flow = true;
      }
      if (!size) {
        complete = false;
        continue;
      }
      reduced = c == 'f' ? std::min(*size, uint64_t(8)) : *size;
      size = std::max(*size, conversion.width);
      reduced = std::max(reduced, conversion.width);
      auto fullSum = sum(maximum, size), limitedSum = sum(limited, reduced);
      if (!fullSum || !limitedSum) {
        complete = false;
        continue;
      }
      maximum = *fullSum;
      limited = *limitedSum;
    }
    if (complete) {
      uint64_t width = 1;
      if (callee && ValueFacts::hasLibraryName(*callee, "swprintf", true))
        width = characterBytes(call.getArgOperand(0));
      auto full = sum(maximum, 1), small = sum(limited, 1);
      result.maximum_bytes = full ? product(*full, width) : None;
      result.maximum_bytes_without_large_floats = small ? product(*small, width) : None;
    }
    return result;
  }

  Optional<MemoryRegion> region(const Value *value, unsigned depth = 48) const {
    if (!depth || !value || !value->getType()->isPointerTy())
      return None;
    const bool top_level = (depth == 48 && region_active.empty());
    if (top_level) {
      auto cached = region_cache.find(value);
      if (cached != region_cache.end())
        return cached->second;
    }
    if (!region_active.insert(value).second)
      return None;
    auto finish = [&](Optional<MemoryRegion> res) {
      region_active.erase(value);
      if (top_level)
        region_cache[value] = res;
      return res;
    };
    const Value *resolved = resolve(value);
    if (resolved != value)
      return finish(region(resolved, depth - 1));
    if (const auto *alias = dyn_cast<GlobalAlias>(value))
      return finish(region(alias->getAliasee(), depth - 1));
    if (const auto *cast = dyn_cast<BitCastOperator>(value))
      return finish(region(cast->getOperand(0), depth - 1));
    if (const auto *cast = dyn_cast<AddrSpaceCastOperator>(value))
      return finish(region(cast->getOperand(0), depth - 1));
    if (const auto *allocation = dyn_cast<AllocaInst>(value)) {
      auto size = typeBytes(allocation->getAllocatedType(), layout);
      auto count = unsignedInteger(allocation->getArraySize());
      if (!size || !count)
        return finish(None);
      auto total = product(*size, *count);
      if (total)
        return finish(MemoryRegion{value, *total, 0, false,
                                   characterArray(allocation->getAllocatedType())});
      return finish(None);
    }
    if (const auto *global = dyn_cast<GlobalVariable>(value)) {
      auto size = typeBytes(global->getValueType(), layout);
      if (size)
        return finish(MemoryRegion{value, *size, 0, false,
                                   characterArray(global->getValueType())});
      return finish(None);
    }
    if (const auto *call = dyn_cast<CallBase>(value)) {
      const Function *target = ValueFacts::callee(*call);
      if (!target)
        return finish(None);
      Optional<uint64_t> size;
      AllocationKind kind = LibraryModels::allocation(*target);
      if ((ValueFacts::hasLibraryName(*target, "malloc", true) ||
           kind == AllocationKind::New || kind == AllocationKind::NewArray) &&
          call->arg_size())
        size = unsignedInteger(call->getArgOperand(0));
      if (ValueFacts::hasLibraryName(*target, "calloc", true) &&
          call->arg_size() >= 2) {
        auto count = unsignedInteger(call->getArgOperand(0));
        auto element = unsignedInteger(call->getArgOperand(1));
        size = count && element ? product(*count, *element) : None;
      }
      if (ValueFacts::hasLibraryName(*target, "realloc", true) &&
          call->arg_size() >= 2)
        size = unsignedInteger(call->getArgOperand(1));
      if (ValueFacts::hasLibraryName(*target, "aligned_alloc", true) &&
          call->arg_size() >= 2)
        size = unsignedInteger(call->getArgOperand(1));
      if (size)
        return finish(MemoryRegion{value, *size, 0, true, false});
      return finish(None);
    }
    if (const auto *gep = dyn_cast<GEPOperator>(value)) {
      auto result = region(gep->getPointerOperand(), depth - 1);
      Type *type = gep->getSourceElementType();
      bool first = true;
      if (!result && type->isStructTy() && gep->getNumIndices() >= 2) {
        const auto *index = gep->idx_begin();
        auto leading = integer(index++->get());
        auto field = integer(index->get());
        auto *structure = cast<StructType>(type);
        if (leading && leading->isZero() && field && !field->isNegative() &&
            field->getActiveBits() <= 32 &&
            field->getZExtValue() < structure->getNumElements()) {
          uint64_t n = field->getZExtValue();
          Type *member = structure->getElementType(n);
          if (member->isArrayTy() && member->getArrayNumElements() > 1) {
            auto bytes = typeBytes(type, layout);
            auto base = address(gep->getPointerOperand());
            if (bytes && base)
              result = MemoryRegion{base->object, *bytes, 0, false, false};
          }
        }
      }
      if (!result)
        return finish(None);
      for (const Use &use : gep->indices()) {
        auto index = integer(use.get());
        if (!index || index->getMinSignedBits() > 64)
          return finish(None);
        int64_t n = index->getSExtValue();
        if (!first && type->isStructTy()) {
          auto *structure = cast<StructType>(type);
          if (n < 0 || static_cast<uint64_t>(n) >= structure->getNumElements())
            return finish(None);
          // Selecting a field narrows the region, preventing access from
          // silently crossing into a following struct member.
          auto enclosing = typeBytes(type, layout);
          if (!enclosing || result->offset < 0 ||
              static_cast<uint64_t>(result->offset) > result->capacity)
            return finish(None);
          const uint64_t fieldOffset =
              layout.getStructLayout(structure)->getElementOffset(n);
          const uint64_t remaining =
              result->capacity - static_cast<uint64_t>(result->offset);
          if (fieldOffset > remaining)
            return finish(None);
          type = structure->getElementType(n);
          auto bytes = typeBytes(type, layout);
          if (!bytes)
            return finish(None);
          // LLVM retains the trailing array shape but not the distinction
          // between a flexible member and the historical one-element idiom.
          // A zero-length tail stays unknown. An overallocated heap object
          // supplies the actual available extent of its one-element tail.
          const auto *array = dyn_cast<ArrayType>(type);
          bool last =
              static_cast<unsigned>(n) + 1 == structure->getNumElements();
          if (last && array && !array->getNumElements())
            return finish(None);
          bool expandedTail = last && array && array->getNumElements() == 1 &&
                              result->heap && remaining > *enclosing;
          // The declared tail-array subobject grows by the bytes beyond the
          // enclosing record, retaining its logical bound rather than silently
          // making padding available as string data.
          result->capacity = expandedTail
                                 ? remaining - *enclosing + *bytes
                                 : std::min(*bytes, remaining - fieldOffset);
          result->offset = 0;
          result->static_character_array =
              !expandedTail && characterArray(type);
          continue;
        }
        Type *element = first               ? type
                        : type->isArrayTy() ? type->getArrayElementType()
                                            : nullptr;
        if (!element)
          return finish(None);
        auto stride = typeBytes(element, layout);
        if (!stride)
          return finish(None);
        APInt offset(128, static_cast<uint64_t>(result->offset), true);
        offset +=
            APInt(128, static_cast<uint64_t>(n), true) * APInt(128, *stride);
        if (offset.getMinSignedBits() > 64)
          return finish(None);
        result->offset = offset.getSExtValue();
        if (!first && element->isArrayTy()) {
          // Only a valid outer index selects an inner array subobject.
          auto *outer = cast<ArrayType>(type);
          if (n < 0 || static_cast<uint64_t>(n) >= outer->getNumElements())
            return finish(result);
          result->capacity = *stride;
          result->offset = 0;
          result->static_character_array = characterArray(element);
        }
        type = element;
        first = false;
      }
      return finish(result);
    }
    // Require every alternative to identify the same region. Mixed origins
    // are unknown, rather than pretending that a may-origin is a proof.
    const auto *phi = dyn_cast<PHINode>(value);
    const auto *select = dyn_cast<SelectInst>(value);
    if (phi || select) {
      Optional<MemoryRegion> merged;
      unsigned count = phi ? phi->getNumIncomingValues() : 2;
      for (unsigned i = 0; i < count; ++i) {
        const Value *input = phi ? phi->getIncomingValue(i)
                             : i ? select->getFalseValue()
                                 : select->getTrueValue();
        auto incoming = region(input, depth - 1);
        if (!incoming)
          return finish(None);
        if (merged && (merged->allocation != incoming->allocation ||
                       merged->capacity != incoming->capacity ||
                       merged->offset != incoming->offset))
          return finish(None);
        merged = incoming;
      }
      return finish(merged);
    }
    return finish(None);
  }

  std::vector<BufferAccess> accesses(const Instruction &site) const {
    std::vector<BufferAccess> result;
    auto add = [&](const Value *pointer, Optional<uint64_t> bytes,
                   bool writes) {
      if (!pointer || !bytes)
        return;
      auto memory = region(pointer);
      if (memory)
        result.push_back({&site, pointer, *bytes, writes, *memory});
    };
    if (const auto *load = dyn_cast<LoadInst>(&site))
      add(load->getPointerOperand(), typeBytes(load->getType(), layout, false),
          false);
    if (const auto *store = dyn_cast<StoreInst>(&site))
      add(store->getPointerOperand(),
          typeBytes(store->getValueOperand()->getType(), layout, false), true);
    if (const auto *atomic = dyn_cast<AtomicRMWInst>(&site))
      add(atomic->getPointerOperand(),
          typeBytes(atomic->getValOperand()->getType(), layout, false), true);
    if (const auto *atomic = dyn_cast<AtomicCmpXchgInst>(&site))
      add(atomic->getPointerOperand(),
          typeBytes(atomic->getCompareOperand()->getType(), layout, false),
          true);
    if (const auto *transfer = dyn_cast<MemTransferInst>(&site)) {
      auto size = unsignedInteger(transfer->getLength());
      add(transfer->getRawDest(), size, true);
      add(transfer->getRawSource(), size, false);
      return result;
    }
    if (const auto *set = dyn_cast<MemSetInst>(&site)) {
      add(set->getRawDest(), unsignedInteger(set->getLength()), true);
      return result;
    }
    const auto *call = dyn_cast<CallBase>(&site);
    const Function *target = call ? ValueFacts::callee(*call) : nullptr;
    if (!target)
      return result;
    auto argument = [&](unsigned index) {
      return ValueFacts::argument(*call, index);
    };
    auto count = [&](unsigned index) -> Optional<uint64_t> {
      const Value *value = argument(index);
      return value ? unsignedInteger(value) : None;
    };
    auto named = [&](const char *name) {
      return ValueFacts::hasLibraryName(*target, name, true, true);
    };
    if (named("memcpy") || named("memmove") || named("mempcpy")) {
      add(argument(0), count(2), true);
      add(argument(1), count(2), false);
    } else if (named("bcopy")) {
      add(argument(1), count(2), true);
      add(argument(0), count(2), false);
    } else if (named("memset") || named("strncpy")) {
      add(argument(0), count(2), true);
    } else if (named("strncat")) {
      auto size = count(2);
      if (size && *size != std::numeric_limits<uint64_t>::max())
        add(argument(0), *size + 1, true);
    } else if (named("memcmp")) {
      add(argument(0), count(2), false);
      add(argument(1), count(2), false);
    } else if (named("read") || named("recv")) {
      add(argument(1), count(2), true);
    } else if (named("write") || named("send")) {
      add(argument(1), count(2), false);
    } else if (named("fgets")) {
      // fgets takes a signed int. A nonpositive count does not request a
      // large unsigned write, unlike the size_t parameters of memory APIs.
      const Value *size = argument(1);
      auto signedSize = size ? integer(size) : None;
      if (signedSize && !signedSize->isNegative())
        add(argument(0), count(1), true);
    } else if (named("snprintf") || named("vsnprintf")) {
      add(argument(0), count(1), true);
    } else if (named("fread") || named("fwrite")) {
      auto elements = count(2), size = count(1);
      add(argument(0), elements && size ? product(*elements, *size) : None,
          named("fread"));
    } else if (named("strcpy") || named("stpcpy")) {
      if (const Value *source = argument(1))
        if (auto text = ValueFacts::constantString(*resolve(source)))
          add(argument(0), text->size() + 1, true);
    }
    return result;
  }

  struct StringLength {
    const CallBase *call = nullptr;
    uint64_t scale = 1;
    int64_t constant = 0;
  };

  Optional<StringLength> lengthCall(const Value *value,
                                    unsigned depth = 32) const {
    if (!depth)
      return None;
    value = resolve(value);
    if (const auto *cast = dyn_cast<CastInst>(value)) {
      if (cast->getType()->isIntegerTy() &&
          cast->getOperand(0)->getType()->isIntegerTy())
        return lengthCall(cast->getOperand(0), depth - 1);
    }
    if (const auto *op = dyn_cast<BinaryOperator>(value)) {
      if (op->getOpcode() == Instruction::Add ||
          op->getOpcode() == Instruction::Sub) {
        for (unsigned i = 0; i < 2; ++i) {
          if (op->getOpcode() == Instruction::Sub && i == 0)
            continue;
          auto number = integer(op->getOperand(i));
          auto length = lengthCall(op->getOperand(1 - i), depth - 1);
          if (number && number->getMinSignedBits() <= 64 && length) {
            APInt total(128, static_cast<uint64_t>(length->constant), true);
            APInt change = number->sextOrTrunc(128);
            total += op->getOpcode() == Instruction::Sub ? -change : change;
            if (total.getMinSignedBits() <= 64)
              return StringLength{length->call, length->scale, total.getSExtValue()};
          }
        }
      }
      if (op->getOpcode() == Instruction::Mul)
        for (unsigned i = 0; i < 2; ++i) {
          auto constant = unsignedInteger(op->getOperand(i));
          auto length = lengthCall(op->getOperand(1 - i), depth - 1);
          if (constant && *constant && length)
            if (auto scale = product(length->scale, *constant)) {
              APInt total(128, static_cast<uint64_t>(length->constant), true);
              total *= APInt(128, *constant);
              if (total.getMinSignedBits() <= 64)
                return StringLength{length->call, *scale, total.getSExtValue()};
            }
        }
      if (op->getOpcode() == Instruction::Shl) {
        auto shift = unsignedInteger(op->getOperand(1));
        auto length = lengthCall(op->getOperand(0), depth - 1);
        if (shift && *shift < 64 && length)
          if (auto scale = product(length->scale, uint64_t(1) << *shift)) {
            APInt total(128, static_cast<uint64_t>(length->constant), true);
            total <<= *shift;
            if (total.getMinSignedBits() <= 64)
              return StringLength{length->call, *scale, total.getSExtValue()};
          }
      }
    }
    const auto *call = dyn_cast<CallBase>(value);
    const Function *target = call ? ValueFacts::callee(*call) : nullptr;
    if (target && call->arg_size() == 1 &&
        (ValueFacts::hasLibraryName(*target, "strlen", true, true) ||
         ValueFacts::hasLibraryName(*target, "wcslen", true, true)))
      return StringLength{call, 1};
    return None;
  }

  const CallBase *allocationOrigin(const Value *pointer,
                                   unsigned depth = 32) const {
    if (!depth)
      return nullptr;
    pointer = resolve(pointer);
    if (const auto *cast = dyn_cast<BitCastOperator>(pointer))
      return allocationOrigin(cast->getOperand(0), depth - 1);
    if (const auto *gep = dyn_cast<GEPOperator>(pointer)) {
      APInt offset(layout.getIndexTypeSizeInBits(pointer->getType()), 0);
      if (!gep->accumulateConstantOffset(layout, offset) || !offset.isZero())
        return nullptr;
      return allocationOrigin(gep->getPointerOperand(), depth - 1);
    }
    const auto *call = dyn_cast<CallBase>(pointer);
    const Function *target = call ? ValueFacts::callee(*call) : nullptr;
    return target &&
                   LibraryModels::allocation(*target) != AllocationKind::Unknown
               ? call
               : nullptr;
  }

  Optional<StringLength> allocationLength(const CallBase &allocation) const {
    const Function *target = ValueFacts::callee(allocation);
    if (!target || !allocation.arg_size())
      return None;
    auto named = [&](const char *name) {
      return ValueFacts::hasLibraryName(*target, name, true);
    };
    auto kind = LibraryModels::allocation(*target);
    if (named("malloc") || kind == AllocationKind::New ||
        kind == AllocationKind::NewArray)
      return lengthCall(allocation.getArgOperand(0));
    if ((named("realloc") || named("aligned_alloc")) &&
        allocation.arg_size() >= 2)
      return lengthCall(allocation.getArgOperand(1));
    if (named("calloc") && allocation.arg_size() >= 2)
      for (unsigned i = 0; i < 2; ++i) {
        auto length = lengthCall(allocation.getArgOperand(i));
        auto multiplier = unsignedInteger(allocation.getArgOperand(1 - i));
        if (length && multiplier && *multiplier)
          if (auto scale = product(length->scale, *multiplier)) {
            APInt total(128, static_cast<uint64_t>(length->constant), true);
            total *= APInt(128, *multiplier);
            if (total.getMinSignedBits() <= 64)
              return StringLength{length->call, *scale, total.getSExtValue()};
          }
      }
    return None;
  }

  struct ScaledValue {
    const Value *value = nullptr;
    uint64_t scale = 1;
  };

  Optional<ScaledValue> scaled(const Value *value, const Instruction &at,
                               unsigned depth = 24) const {
    if (!depth || !value->getType()->isIntegerTy())
      return None;
    value = resolve(value);
    if (const auto *cast = dyn_cast<CastInst>(value)) {
      if (cast->getOpcode() == Instruction::ZExt ||
          (cast->getOpcode() == Instruction::SExt &&
           state(*at.getFunction())
               .facts.nonNegative(*cast->getOperand(0), at)))
        return scaled(cast->getOperand(0), at, depth - 1);
    }
    if (const auto *op = dyn_cast<BinaryOperator>(value)) {
      if (op->getOpcode() == Instruction::Mul)
        for (unsigned i = 0; i < 2; ++i) {
          auto factor = unsignedInteger(op->getOperand(i));
          auto base = scaled(op->getOperand(1 - i), at, depth - 1);
          if (factor && *factor && base)
            if (auto total = product(*factor, base->scale))
              return ScaledValue{base->value, *total};
        }
      if (op->getOpcode() == Instruction::Shl) {
        auto shift = unsignedInteger(op->getOperand(1));
        auto base = scaled(op->getOperand(0), at, depth - 1);
        if (shift && *shift < 64 && base)
          if (auto total = product(uint64_t(1) << *shift, base->scale))
            return ScaledValue{base->value, *total};
      }
    }
    return ScaledValue{value, 1};
  }

  bool allocationEqualsIndex(const CallBase &allocation, const Value &index,
                             uint64_t stride, const Instruction &at) const {
    const Function *target = ValueFacts::callee(allocation);
    if (!target || !allocation.arg_size())
      return false;
    auto kind = LibraryModels::allocation(*target);
    const Value *size = nullptr;
    uint64_t factor = 1;
    if (ValueFacts::hasLibraryName(*target, "malloc", true) ||
        kind == AllocationKind::New || kind == AllocationKind::NewArray)
      size = allocation.getArgOperand(0);
    if ((ValueFacts::hasLibraryName(*target, "realloc", true) ||
         ValueFacts::hasLibraryName(*target, "aligned_alloc", true)) &&
        allocation.arg_size() >= 2)
      size = allocation.getArgOperand(1);
    if (ValueFacts::hasLibraryName(*target, "calloc", true) &&
        allocation.arg_size() >= 2)
      for (unsigned i = 0; i < 2; ++i)
        if (auto element = unsignedInteger(allocation.getArgOperand(i))) {
          size = allocation.getArgOperand(1 - i);
          factor = *element;
          break;
        }
    if (!size || integer(size))
      return false; // Constant extents are handled by MemoryRegion.
    auto length = scaled(size, at), offset = scaled(&index, at);
    if (!length || !offset)
      return false;
    auto lengthScale = product(length->scale, factor);
    auto offsetScale = product(offset->scale, stride);
    return lengthScale && offsetScale && *lengthScale == *offsetScale &&
           state(*at.getFunction())
               .facts.equivalent(*length->value, *offset->value);
  }

  struct AllocationEnd {
    const CallBase *allocation = nullptr;
    int64_t delta = 0;
  };

  Optional<AllocationEnd> allocationEnd(const Value *pointer,
                                        const Instruction &at,
                                        unsigned depth = 32) const {
    if (!depth)
      return None;
    pointer = resolve(pointer);
    if (const auto *cast = dyn_cast<BitCastOperator>(pointer))
      return allocationEnd(cast->getOperand(0), at, depth - 1);
    const auto *gep = dyn_cast<GEPOperator>(pointer);
    if (!gep)
      return None;
    APInt displacement(layout.getIndexTypeSizeInBits(pointer->getType()), 0);
    if (gep->accumulateConstantOffset(layout, displacement)) {
      auto base = allocationEnd(gep->getPointerOperand(), at, depth - 1);
      if (!base || displacement.getMinSignedBits() > 64)
        return None;
      APInt delta(128, static_cast<uint64_t>(base->delta), true);
      delta += displacement.sextOrTrunc(128);
      if (delta.getMinSignedBits() > 64)
        return None;
      return AllocationEnd{base->allocation, delta.getSExtValue()};
    }
    // A dynamic pointer index is compared semantically to the byte count
    // allocated. It is a relative bound, not a guessed integer range.
    if (gep->getNumIndices() != 1)
      return None;
    const auto *allocation = allocationOrigin(gep->getPointerOperand());
    auto stride = typeBytes(gep->getSourceElementType(), layout);
    if (allocation && stride &&
        allocationEqualsIndex(*allocation, *gep->idx_begin()->get(), *stride,
                              at))
      return AllocationEnd{allocation, 0};
    return None;
  }

  bool rangeGuardCondition(const Value *condition, bool truth,
                            const Value *value, const Instruction &at,
                            unsigned depth = 16) const {
    if (!depth)
      return false;
    auto &analysis = state(*at.getFunction());
    auto canonical = [&](const Value *input) {
      input = resolve(input);
      while (const auto *cast = dyn_cast<CastInst>(input)) {
        if (cast->getOpcode() != Instruction::SExt &&
            cast->getOpcode() != Instruction::ZExt)
          break;
        input = resolve(cast->getOperand(0));
      }
      return input;
    };
    if (const auto *cmp = dyn_cast<ICmpInst>(condition)) {
      auto predicate = truth ? cmp->getPredicate() : cmp->getInversePredicate();
      if (predicate == ICmpInst::ICMP_NE)
        return false;
      const Value *other = nullptr;
      if (analysis.facts.equivalent(*canonical(value), *canonical(cmp->getOperand(0))))
        other = cmp->getOperand(1);
      else if (analysis.facts.equivalent(*canonical(value), *canonical(cmp->getOperand(1))))
        other = cmp->getOperand(0);
      if (!other)
        return false;
      auto bound = integer(other);
      return !bound || !bound->isZero();
    }
    if (const auto *op = dyn_cast<BinaryOperator>(condition)) {
      if ((truth && op->getOpcode() == Instruction::And) ||
          (!truth && op->getOpcode() == Instruction::Or))
        return rangeGuardCondition(op->getOperand(0), truth, value, at, depth - 1) ||
               rangeGuardCondition(op->getOperand(1), truth, value, at, depth - 1);
      if (op->getOpcode() == Instruction::Xor)
        if (const auto *constant = dyn_cast<ConstantInt>(op->getOperand(1)))
          if (constant->isOne())
            return rangeGuardCondition(op->getOperand(0), !truth, value, at, depth - 1);
    }
    if (const auto *phi = dyn_cast<PHINode>(condition)) {
      bool possible = false;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        const Value *input = phi->getIncomingValue(i);
        if (const auto *constant = dyn_cast<ConstantInt>(input))
          if (constant->isZero() == truth)
            continue;
        possible = true;
        if (!rangeGuardCondition(input, truth, value, at, depth - 1))
          return false;
      }
      return possible;
    }
    return false;
  }

  bool guardOnEdge(const Value *value, const BasicBlock &from,
                    const BasicBlock &to) const {
    const auto *branch = dyn_cast<BranchInst>(from.getTerminator());
    if (!branch || !branch->isConditional() ||
        (branch->getSuccessor(0) != &to && branch->getSuccessor(1) != &to))
      return false;
    return rangeGuardCondition(branch->getCondition(),
                                branch->getSuccessor(0) == &to,
                                value, *branch);
  }

  bool hasRangeGuard(const Value *value, const Instruction &at) const {
    value = resolve(value);
    auto &analysis = state(*at.getFunction());
    const auto *domNode =
        analysis.dominators.getNode(const_cast<BasicBlock *>(at.getParent()));
    if (!domNode)
      return false;
    for (const auto *cur = domNode->getIDom(); cur; cur = cur->getIDom()) {
      const BasicBlock &block = *cur->getBlock();
      const auto *branch = dyn_cast<BranchInst>(block.getTerminator());
      if (!branch || !branch->isConditional())
        continue;
      bool yes = analysis.dominators.dominates(
          BasicBlockEdge(branch->getParent(), branch->getSuccessor(0)), at.getParent());
      bool no = analysis.dominators.dominates(
          BasicBlockEdge(branch->getParent(), branch->getSuccessor(1)), at.getParent());
      if (yes == no)
        continue;
      if (rangeGuardCondition(branch->getCondition(), yes, value, at))
        return true;
    }
    return false;
  }

  // Call-independent portion of nullArguments(target, depth): memoized by
  // (Function *, depth) to avoid O(fanout^4) re-traversal of the call graph.
  const std::vector<unsigned> &
  nullArgumentsForFunction(const Function &target, unsigned depth) const {
    static const std::vector<unsigned> kEmpty;
    if (!depth)
      return kEmpty;
    auto key = std::make_pair(&target, depth);
    auto cached = null_args_cache.find(key);
    if (cached != null_args_cache.end())
      return cached->second;
    // Insert empty sentinel before recursing to guard direct/mutual recursion
    // and ensure cache presence; overwrite with final result below.
    std::set<unsigned> result;
    auto named = [&](const char *name) {
      return ValueFacts::hasLibraryName(target, name, true, true);
    };
    for (const char *name : {"strlen", "wcslen", "strdup", "strchr", "strrchr",
                             "atoi", "atol", "atoll", "atof", "strtol", "strtoul",
                             "puts", "fputs"})
      if (named(name))
        result.insert(0);
    if (named("strcmp") || named("wcscmp") || named("strstr") || named("wcsstr")) {
      result.insert(0);
      result.insert(1);
    }
    if (named("strcpy") || named("stpcpy") || named("wcscpy"))
      result.insert(1);
    if (named("strcat") || named("wcscat")) {
      result.insert(0);
      result.insert(1);
    }
    if (named("strncat") || named("wcsncat"))
      result.insert(0);
    if (named("sscanf"))
      result.insert(0);
    if (!target.isDeclaration() && depth > 1) {
      for (const BasicBlock &block : target) {
        for (const Instruction &inst : block) {
          if (const auto *child = dyn_cast<CallBase>(&inst)) {
            for (unsigned index : nullArguments(*child, depth - 1)) {
              if (index >= child->arg_size())
                continue;
              const Value *argument = child->getArgOperand(index);
              auto origin = address(argument);
              const auto *parameter =
                  origin ? dyn_cast<Argument>(origin->object) : nullptr;
              if (!parameter || parameter->getParent() != &target)
                continue;
              if (stringState(argument, *child).bounds.termination ==
                  StringTermination::Terminated)
                continue;
              result.insert(parameter->getArgNo());
            }
          }
        }
      }
    }
    std::vector<unsigned> vec(result.begin(), result.end());
    return null_args_cache.try_emplace(key, std::move(vec)).first->second;
  }

  std::vector<unsigned> nullArguments(const CallBase &call,
                                      unsigned depth = 4) const {
    const Function *target = ValueFacts::callee(call);
    if (!target || !depth)
      return {};
    const auto &base = nullArgumentsForFunction(*target, depth);
    auto model = LibraryModels::format(*target);
    if (!model || model->format >= call.arg_size()) {
      std::vector<unsigned> valid;
      valid.reserve(base.size());
      for (unsigned index : base)
        if (index < call.arg_size() &&
            call.getArgOperand(index)->getType()->isPointerTy())
          valid.push_back(index);
      return valid;
    }
    std::set<unsigned> result(base.begin(), base.end());
    result.insert(model->format);
    if (!model->scanf)
      if (auto format = printFormat(call))
        for (const PrintConversion &conversion : format->conversions)
          if (std::tolower(conversion.conversion) == 's' &&
              !conversion.precision)
            result.insert(conversion.argument);
    std::vector<unsigned> valid;
    valid.reserve(result.size());
    for (unsigned index : result)
      if (index < call.arg_size() &&
          call.getArgOperand(index)->getType()->isPointerTy())
        valid.push_back(index);
    return valid;
  }

  const CallBase *uncheckedSnprintfResult(const Value *value,
                                         const Instruction &at,
                                         bool arithmetic,
                                         unsigned depth,
                                         std::set<std::pair<const Value *, const BasicBlock *>> &seen) const {
    if (!depth || !seen.insert({value, at.getParent()}).second ||
        hasRangeGuard(value, at))
      return nullptr;
    if (const auto *call = dyn_cast<CallBase>(value)) {
      const Function *target = ValueFacts::callee(*call);
      if (arithmetic && target &&
          (ValueFacts::hasLibraryName(*target, "snprintf", true, true) ||
           ValueFacts::hasLibraryName(*target, "vsnprintf", true, true) ||
           ValueFacts::hasLibraryName(*target, "snprintf_s", true, true)))
        return call;
      return nullptr;
    }
    if (const auto *load = dyn_cast<LoadInst>(value)) {
      if (load->isAtomic() || load->isVolatile())
        return nullptr;
      auto &analysis = state(*load->getFunction());
      std::set<MemoryAccess *> visited;
      std::function<const CallBase *(MemoryAccess *, const Instruction &, unsigned)> walk;
      walk = [&](MemoryAccess *access, const Instruction &context,
                 unsigned remaining) -> const CallBase * {
        if (!remaining || !access)
          return nullptr;
        access = analysis.memory->getWalker()->getClobberingMemoryAccess(
            access, MemoryLocation::get(load));
        if (!visited.insert(access).second)
          return nullptr;
        if (auto *phi = dyn_cast<MemoryPhi>(access)) {
          for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            auto *incoming = analysis.memory->getWalker()->getClobberingMemoryAccess(
                phi->getIncomingValue(i), MemoryLocation::get(load));
            const auto *definition = dyn_cast<MemoryDef>(incoming);
            const auto *store = definition
                                    ? dyn_cast_or_null<StoreInst>(definition->getMemoryInst())
                                    : nullptr;
            if (store && guardOnEdge(store->getValueOperand(),
                                     *phi->getIncomingBlock(i), *phi->getBlock()))
              continue;
            if (const CallBase *source = walk(phi->getIncomingValue(i),
                    *phi->getIncomingBlock(i)->getTerminator(), remaining - 1))
              return source;
          }
          return nullptr;
        }
        const auto *definition = dyn_cast<MemoryDef>(access);
        const auto *store = definition
                                ? dyn_cast_or_null<StoreInst>(definition->getMemoryInst())
                                : nullptr;
        if (!store || analysis.aliases.alias(MemoryLocation::get(store),
                                             MemoryLocation::get(load)) != AliasResult::MustAlias)
          return nullptr;
        return uncheckedSnprintfResult(store->getValueOperand(), context,
                                        arithmetic, remaining - 1, seen);
      };
      return walk(analysis.memory->getWalker()->getClobberingMemoryAccess(load),
                  at, depth - 1);
    }
    if (const auto *phi = dyn_cast<PHINode>(value)) {
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        if (guardOnEdge(phi->getIncomingValue(i), *phi->getIncomingBlock(i),
                        *phi->getParent()))
          continue;
        if (const CallBase *source = uncheckedSnprintfResult(phi->getIncomingValue(i),
                *phi->getIncomingBlock(i)->getTerminator(), arithmetic, depth - 1, seen))
          return source;
      }
      return nullptr;
    }
    if (const auto *instruction = dyn_cast<Instruction>(value)) {
      bool expression = isa<BinaryOperator>(instruction) || isa<CastInst>(instruction) ||
                        isa<GetElementPtrInst>(instruction) || isa<SelectInst>(instruction);
      if (!expression)
        return nullptr;
      bool risky = isa<BinaryOperator>(instruction) || isa<GetElementPtrInst>(instruction);
      if (const auto *cast = dyn_cast<CastInst>(instruction))
        risky |= cast->getOpcode() == Instruction::Trunc;
      for (const Use &operand : instruction->operands())
        if (const CallBase *source = uncheckedSnprintfResult(operand.get(), at,
                arithmetic || risky, depth - 1, seen))
          return source;
    }
    return nullptr;
  }

  using FindingEmitter = std::function<void(StringRef, const Instruction *,
                         std::string, std::vector<const Value *>)>;

  void callChecks(const CallBase &call, const TaintFlowResult *taint,
                  const FindingEmitter &emit) const {
    const CallBase *copy = &call;
    const Instruction &inst = call;
    const Function *target = ValueFacts::callee(call);
    if (!target)
      return;
    auto &analysis = state(*call.getFunction());
    auto named = [&](const char *name) {
      return ValueFacts::hasLibraryName(*target, name, true, true);
    };
    auto userOrigins = [&](unsigned argument) {
      if (!taint || argument >= copy->arg_size())
        return std::vector<TaintOrigin>();
      auto origins = taint->originsAt(inst, *copy->getArgOperand(argument),
                                     TaintChannel::Memory);
      origins.erase(std::remove_if(origins.begin(), origins.end(),
          [](const TaintOrigin &origin) { return origin.nonconstant_only; }),
          origins.end());
      return origins;
    };
    for (unsigned argument : nullArguments(*copy)) {
      const Value *pointer = copy->getArgOperand(argument);
      auto contents = stringState(pointer, inst);
      if (contents.bounds.termination != StringTermination::Unproven)
        continue;
      std::vector<const Value *> evidence{pointer};
      for (const Instruction *source : contents.bounds.evidence)
        evidence.push_back(source);
      bool inputWrite = false;
      for (const Instruction *source : contents.bounds.evidence)
        if (const auto *input = dyn_cast<CallBase>(source))
          if (const Function *reader = ValueFacts::callee(*input))
            for (const char *name : {"read", "recv", "fread"})
              inputWrite |= ValueFacts::hasLibraryName(*reader, name, true);
      if (inputWrite) {
        auto origins = userOrigins(argument);
        if (!origins.empty())
          emit("cpp/user-controlled-null-termination-tainted", copy,
               "Input fills this string buffer without an established "
               "terminator before its string-consuming use.", evidence);
      } else
        emit("cpp/improper-null-termination", copy,
             "This tracked local buffer has no established null terminator "
             "before its string-consuming use.", evidence);
    }
    bool append = named("strcat") || named("wcscat") ||
                  named("strncat") || named("wcsncat");
    bool unboundedCopy = named("strcpy") || named("stpcpy") ||
                         named("wcscpy") || named("strcat") || named("wcscat");
    bool formattedWrite = named("sprintf") || named("vsprintf") || named("wsprintf");
    if ((unboundedCopy && copy->arg_size() >= 2) ||
        (formattedWrite && copy->arg_size() >= 2)) {
      FormatBounds output;
      if (formattedWrite)
        output = formatBounds(*copy);
      else {
        auto input = stringState(copy->getArgOperand(1), inst);
        output.maximum_bytes = input.bounds.maximum_bytes;
        if (append)
          output.maximum_bytes = sum(output.maximum_bytes,
              stringState(copy->getArgOperand(0), inst).bounds.maximum_bytes);
        output.maximum_bytes = sum(output.maximum_bytes,
                                    characterBytes(copy->getArgOperand(0)));
        output.maximum_bytes_without_large_floats = output.maximum_bytes;
        output.value_flow = true;
        if (!input.bounds.maximum_bytes)
          output.unbounded_string_arguments.push_back(1);
      }
      auto capacity = available(copy->getArgOperand(0));
      if (capacity && output.maximum_bytes && *output.maximum_bytes > *capacity) {
        const char *rule = "cpp/overrunning-write";
        if (output.floating_conversion && output.maximum_bytes_without_large_floats &&
            *output.maximum_bytes_without_large_floats <= *capacity)
          rule = "cpp/overrunning-write-with-float";
        else if (output.value_flow)
          rule = "cpp/very-likely-overrunning-write";
        emit(rule, copy,
             "This unbounded string write may require " +
                 std::to_string(*output.maximum_bytes) + " bytes including "
                 "termination; the destination has " + std::to_string(*capacity) + ".",
             {copy->getArgOperand(0)});
        if (formattedWrite)
          emit("cpp/potential-buffer-overflow", copy,
               "The formatted output bound exceeds the destination's capacity.",
               {copy->getArgOperand(0), copy->getArgOperand(1)});
      }
      if (!output.maximum_bytes && taint) {
        auto model = LibraryModels::format(*target);
        if (formattedWrite && model)
          output.unbounded_string_arguments.push_back(model->format);
        for (unsigned source : output.unbounded_string_arguments) {
          auto origins = userOrigins(source);
          if (origins.empty())
            continue;
          std::vector<const Value *> evidence{copy->getArgOperand(source)};
          for (const auto &origin : origins)
            if (origin.source)
              evidence.push_back(origin.source);
          emit("cpp/unbounded-write", copy,
               "User-controlled string content reaches a write with no "
               "explicit output limit or established content-length upper bound.", evidence);
        }
      }
    }
    int limitArgument = -1;
    if (named("strncpy") || named("wcsncpy") || named("strncat") || named("wcsncat"))
      limitArgument = 2;
    if (named("fgets") || named("fgetws") || named("snprintf") ||
        named("vsnprintf") || named("swprintf") || named("vswprintf"))
      limitArgument = 1;
    if (limitArgument >= 0 && static_cast<unsigned>(limitArgument) < copy->arg_size()) {
      auto capacity = available(copy->getArgOperand(0));
      auto count = unsignedInteger(copy->getArgOperand(limitArgument));
      if (named("fgets") || named("fgetws")) {
        auto signedCount = integer(copy->getArgOperand(limitArgument));
        if (signedCount && signedCount->isNegative())
          count = None;
      }
      auto bytes = count ? product(*count, characterBytes(copy->getArgOperand(0))) : None;
      if (capacity && bytes && *bytes > *capacity)
        emit("cpp/badly-bounded-write", copy,
             "The explicit string-write limit is " + std::to_string(*bytes) +
                 " bytes but the destination has only " + std::to_string(*capacity) + ".",
             {copy->getArgOperand(0), copy->getArgOperand(limitArgument)});
    }
    if ((named("snprintf") || named("vsnprintf") || named("snprintf_s")) && copy->arg_size() >= 2) {
      std::set<std::pair<const Value *, const BasicBlock *>> seen;
      if (const CallBase *source = uncheckedSnprintfResult(copy->getArgOperand(1),
              inst, false, 64, seen))
        emit("cpp/overflowing-snprintf", copy,
             "This size derives through unchecked arithmetic from snprintf's "
             "full output length, which can exceed the previously available space.",
             {source, copy->getArgOperand(1)});
    }
    auto scanfModel = LibraryModels::format(*target);
    if (named("gets") && copy->arg_size())
      emit("cpp/unbounded-write", copy,
           "gets reads input without an output bound.", {copy->getArgOperand(0)});
    if (scanfModel && scanfModel->scanf && scanfModel->format < copy->arg_size()) {
      auto text = ValueFacts::constantString(*copy->getArgOperand(scanfModel->format));
      auto format = text ? parseFormat(*text, true) : None;
      if (format)
        for (const auto &conversion : format->conversions) {
          if (conversion.conversion != 's')
            continue;
          unsigned destination = scanfModel->first_argument + conversion.argument - 1;
          if (destination >= copy->arg_size())
            continue;
          size_t position = conversion.offset + 1;
          size_t start = position;
          while (position < text->size() && std::isdigit((*text)[position]))
            ++position;
          if (position < text->size() && (*text)[position] == '$') {
            start = ++position;
            while (position < text->size() && std::isdigit((*text)[position]))
              ++position;
          }
          Optional<uint64_t> width;
          if (position > start) {
            uint64_t parsed = 0;
            if (!StringRef(*text).slice(start, position).getAsInteger(10, parsed))
              width = parsed;
          }
          const Value *pointer = copy->getArgOperand(destination);
          auto capacity = available(pointer);
          auto bytes = width ? sum(*width, 1) : None;
          if (bytes)
            bytes = product(*bytes, characterBytes(pointer));
          if (capacity && bytes && *bytes > *capacity)
            emit("cpp/very-likely-overrunning-write", copy,
                 "The scanf field width and its terminator exceed this destination.", {pointer});
          if (!width && (!named("sscanf") || !userOrigins(0).empty()))
            emit("cpp/unbounded-write", copy,
                 "This input string conversion has no field width.", {pointer});
        }
    }
    if (copy->arg_size() < 2)
      return;
    if (append) {
      const auto *allocation = allocationOrigin(copy->getArgOperand(0));
      auto size = allocation ? allocationLength(*allocation) : None;
      auto old = stringState(copy->getArgOperand(0), inst);
      if (size && size->constant > 0 && old.copied_source && old.copy_site &&
          analysis.facts.equivalent(*resolve(size->call->getArgOperand(0)), *old.copied_source)) {
        auto input = stringState(copy->getArgOperand(1), inst);
        auto amount = sum(input.bounds.maximum_bytes, characterBytes(copy->getArgOperand(0)));
        if (!amount || *amount > static_cast<uint64_t>(size->constant))
          emit("cpp/overflow-calculated", copy,
               "The allocation's string-length expression reserves insufficient "
               "additional space for this append and its terminator.",
               {allocation, old.copy_site, size->call});
      }
    }
    if (taint && (named("memcpy") || named("memmove") ||
                  named("strncpy") || named("strncat")) && copy->arg_size() >= 3) {
      auto length = lengthCall(copy->getArgOperand(2));
      if (length && analysis.facts.equivalent(*resolve(length->call->getArgOperand(0)),
                                             *resolve(copy->getArgOperand(1))) &&
          !hasRangeGuard(copy->getArgOperand(2), inst)) {
        auto origins = userOrigins(1);
        if (!origins.empty()) {
          std::vector<const Value *> evidence{length->call, copy->getArgOperand(1)};
          for (const auto &origin : origins)
            if (origin.source)
              evidence.push_back(origin.source);
          emit("cpp/overflow-destination", copy,
               "User-controlled content is copied using a length derived from "
               "the source rather than a destination-capacity bound.", evidence);
        }
      }
    }
  }

  std::vector<AnalysisFinding> analyze(const TaintFlowResult *taint = nullptr) const {
    std::vector<AnalysisFinding> result;
    std::set<std::pair<std::string, const Instruction *>> emitted;
    auto emit = [&](StringRef id, const Instruction *site, std::string message,
                    std::vector<const Value *> evidence) {
      if (emitted.insert({id.str(), site}).second) {
        std::vector<const Instruction *> instructions;
        for (const Value *value : evidence)
          if (const auto *instruction = dyn_cast<Instruction>(value))
            instructions.push_back(instruction);
        result.push_back(
            {id.str(), site, std::move(message), std::move(instructions)});
      }
    };
    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      auto &analysis = state(function);
      for (const BasicBlock &block : function) {
        if (!analysis.dominators.isReachableFromEntry(&block))
          continue;
        for (const Instruction &inst : block) {
          for (const BufferAccess &access : accesses(inst)) {
            if (!access.outOfBounds())
              continue;
            std::string message =
                std::string(access.writes ? "Write" : "Read") + " of " +
                std::to_string(access.bytes) + " bytes at byte offset " +
                std::to_string(access.region.offset) +
                " exceeds the object's " +
                std::to_string(access.region.capacity) + " byte region.";
            std::vector<const Value *> evidence{access.region.allocation,
                                                access.pointer};
            emit("cpp/overflow-buffer", &inst, message, evidence);
            if (access.region.static_character_array &&
                access.region.offset >= 0)
              emit("cpp/static-buffer-overflow", &inst, message, evidence);
            if (access.region.heap &&
                (isa<LoadInst>(inst) || isa<StoreInst>(inst)))
              emit("cpp/invalid-pointer-deref", &inst, message, evidence);
            const auto *stack = dyn_cast<AllocaInst>(access.region.allocation);
            const auto *global = dyn_cast<GlobalVariable>(access.region.allocation);
            Type *arrayType = stack ? stack->getAllocatedType()
                                   : global ? global->getValueType() : nullptr;
            if (arrayType && arrayType->isArrayTy() &&
                arrayType->getArrayNumElements() > 1 &&
                (isa<LoadInst>(inst) || isa<StoreInst>(inst)))
              emit("cpp/constant-array-overflow", &inst, message, evidence);
          }
          const Value *dereferenced = nullptr;
          Type *accessType = nullptr;
          if (const auto *load = dyn_cast<LoadInst>(&inst)) {
            dereferenced = load->getPointerOperand();
            accessType = load->getType();
          } else if (const auto *store = dyn_cast<StoreInst>(&inst)) {
            dereferenced = store->getPointerOperand();
            accessType = store->getValueOperand()->getType();
          }
          if (dereferenced) {
            // Prove a dynamic access bad only when its entire conservative
            // index range is outside a real array object. A range straddling
            // the boundary is not evidence of an executable bad index.
            const auto *indexed = dyn_cast<GEPOperator>(
                resolve(dereferenced)->stripPointerCasts());
            if (indexed && indexed->getNumIndices() == 2) {
              const auto *array =
                  dyn_cast<ArrayType>(indexed->getSourceElementType());
              const auto *index = indexed->idx_begin();
              const auto *zero = dyn_cast<ConstantInt>(index++->get());
              const Value *subscript = index->get();
              const Value *base =
                  indexed->getPointerOperand()->stripPointerCasts();
              const auto *stack = dyn_cast<AllocaInst>(base);
              const auto *global = dyn_cast<GlobalVariable>(base);
              bool object = (stack && stack->getAllocatedType() == array) ||
                            (global && global->getValueType() == array);
              auto access_bytes = typeBytes(accessType, layout, false);
              if (array && object && zero && zero->isZero() &&
                  !isa<ConstantInt>(subscript) && array->getNumElements() &&
                  access_bytes && *access_bytes) {
                auto range = analysis.facts.integerRangeAt(*subscript, inst);
                // GEP truncates/sign-extends indices to the target pointer
                // index width, independently of the source integer width.
                unsigned width = layout.getIndexSizeInBits(
                    indexed->getPointerAddressSpace());
                if (range)
                  range = range->sextOrTrunc(width);
                ConstantRange valid = ConstantRange::makeExactICmpRegion(
                    ICmpInst::ICMP_SGE, APInt(width, 0));
                if (width <= 64 &&
                    array->getNumElements() <=
                        APInt::getSignedMaxValue(width).getZExtValue())
                  valid =
                      valid.intersectWith(ConstantRange::makeExactICmpRegion(
                          ICmpInst::ICMP_ULT,
                          APInt(width, array->getNumElements())));
                if (range && range->intersectWith(valid).isEmptySet())
                  emit("cpp/overflow-buffer", &inst,
                       "Every index in the guarded dynamic range is outside "
                       "this array's " +
                           std::to_string(array->getNumElements()) +
                           " elements.",
                       {dereferenced, subscript});
              }
            }
            auto end = allocationEnd(dereferenced, inst);
            auto bytes = typeBytes(accessType, layout, false);
            if (end && bytes && *bytes) {
              APInt accessed(128, static_cast<uint64_t>(end->delta), true);
              accessed += APInt(128, *bytes);
              if (accessed.isStrictlyPositive())
                emit("cpp/invalid-pointer-deref", &inst,
                     "This dereference reaches beyond the allocation's "
                     "symbolic byte extent (offset from its end: " +
                         std::to_string(end->delta) + " bytes).",
                     {end->allocation, dereferenced});
            }
            const auto *gep = dyn_cast<GEPOperator>(
                resolve(dereferenced)->stripPointerCasts());
            if (taint && gep && gep->getNumIndices()) {
              const Value *index = (gep->idx_end() - 1)->get();
              auto origins = taint->originsAt(inst, *index, TaintChannel::Value);
              bool userInput = std::any_of(origins.begin(), origins.end(),
                  [](const TaintOrigin &origin) { return !origin.nonconstant_only; });
              if (userInput && index->getType()->isIntegerTy() &&
                  !hasRangeGuard(index, inst)) {
                auto indexRange = integerRange(index, inst, true);
                auto upper = indexRange.getSignedMax();
                auto parent = region(gep->getPointerOperand());
                auto width = typeBytes(gep->getResultElementType(), layout);
                bool safe = parent && width && *width && !upper.isNegative() &&
                            upper.getActiveBits() <= 64 &&
                            upper.getZExtValue() < parent->capacity / *width &&
                            (!indexRange.getSignedMin().isNegative() ||
                             analysis.facts.nonNegative(*index, inst));
                if (!safe) {
                  std::vector<const Value *> evidence{gep, index};
                  for (const auto &origin : origins)
                    if (!origin.nonconstant_only && origin.source)
                      evidence.push_back(origin.source);
                  emit("cpp/unclear-array-index-validation", &inst,
                       "User-controlled array index reaches this access without a "
                       "controlling range/equality guard or an in-bounds range proof.", evidence);
                }
              }
            }
          }
          const auto *copy = dyn_cast<CallBase>(&inst);
          const Function *target = copy ? ValueFacts::callee(*copy) : nullptr;
          if (copy)
            callChecks(*copy, taint, emit);
          if (!target || copy->arg_size() < 2)
            continue;
          bool stringCopy =
              ValueFacts::hasLibraryName(*target, "strcpy", true, true) ||
              ValueFacts::hasLibraryName(*target, "stpcpy", true, true) ||
              ValueFacts::hasLibraryName(*target, "wcscpy", true, true) ||
              ValueFacts::hasLibraryName(*target, "strcat", true, true) ||
              ValueFacts::hasLibraryName(*target, "wcscat", true, true);
          const Value *copySource =
              stringCopy ? copy->getArgOperand(1) : nullptr;
          if (ValueFacts::hasLibraryName(*target, "sprintf", true, true) &&
              copy->arg_size() == 3) {
            auto format = ValueFacts::constantString(*copy->getArgOperand(1));
            if (format && *format == "%s")
              copySource = copy->getArgOperand(2);
          }
          if (!copySource)
            continue;
          const auto *allocation = allocationOrigin(copy->getArgOperand(0));
          auto length = allocation ? allocationLength(*allocation) : None;
          if (!length || !analysis.dominators.dominates(allocation, copy))
            continue;
          const Value *source = resolve(copySource);
          const Value *measured = resolve(length->call->getArgOperand(0));
          // wcslen counts characters; malloc counts bytes. Use the target
          // pointee type rather than assuming a host wchar_t width.
          auto width =
              measured->getType()->isPointerTy()
                  ? typeBytes(measured->getType()->getPointerElementType(),
                              layout)
                  : None;
          if (!width || length->scale > *width || length->constant != 0)
            continue;
          if (analysis.facts.equivalent(*source, *measured))
            emit("cpp/no-space-for-terminator", allocation,
                 "This string-length-sized allocation has no space reserved "
                 "for the "
                 "terminating zero written by the string copy.",
                 {length->call, copy, source});
        }
      }
    }
    return result;
  }
};

BoundsAnalysis::BoundsAnalysis() = default;
BoundsAnalysis::BoundsAnalysis(Module &module) : impl_(new Impl(module)) {}
BoundsAnalysis::~BoundsAnalysis() = default;
Optional<MemoryRegion> BoundsAnalysis::region(const Value &pointer) const {
  return impl_ ? impl_->region(&pointer) : None;
}
std::vector<BufferAccess> BoundsAnalysis::accesses(const Instruction &site) const {
  return impl_ ? impl_->accesses(site) : std::vector<BufferAccess>();
}
StringBounds BoundsAnalysis::stringBounds(const Value &pointer,
                                       const Instruction &at) const {
  return impl_ ? impl_->stringState(&pointer, at).bounds : StringBounds{};
}
FormatBounds BoundsAnalysis::formatBounds(const CallBase &call) const {
  return impl_ ? impl_->formatBounds(call) : FormatBounds{};
}
std::vector<AnalysisFinding> BoundsAnalysis::analyze() const {
  return impl_ ? impl_->analyze() : std::vector<AnalysisFinding>();
}

const std::vector<AnalysisRuleDescriptor> &BoundsAnalysis::catalog() {
  static const std::vector<AnalysisRuleDescriptor> rules{
      {"cpp/overflow-buffer", "warning",
       "Security/CWE/CWE-119/OverflowBuffer.ql",
       "Known object/subobject capacity, constant byte offset and access size; "
       "loads/stores/atomics, memory intrinsics, and modeled C buffer APIs. "
       "Local pointer spills use must-alias dominating MemorySSA definitions. "
       "Dynamic two-index GEPs on real stack/global arrays are checked when "
       "SCEV and dominating constant guards place the entire index range "
       "outside the array. Straddling ranges and cross-call origins remain "
       "unknown."},
      {"cpp/static-buffer-overflow", "warning", "Critical/OverflowStatic.ql",
       "Statically sized char arrays and char array members with constant "
       "out-of-bounds dereferences or overlarge modeled API lengths. Loop "
       "induction and source macro exclusions are not yet modeled."},
      {"cpp/invalid-pointer-deref", "error",
       "Security/CWE/CWE-193/InvalidPointerDeref.ql",
       "Load/store of known-size malloc/calloc/realloc/new regions using "
       "constant out-of-bounds offsets, plus symbolic allocation-size equals "
       "pointer-offset relations with matching byte/element scaling. Constant "
       "offsets from that end are supported. Constructing a one-past pointer "
       "is not reported. Loop ranges and cross-call pointer flow remain "
       "unknown."},
      {"cpp/no-space-for-terminator", "error",
       "Security/CWE/CWE-131/NoSpaceForZeroTerminator.ql",
       "malloc/calloc/realloc/aligned_alloc/new with strlen/wcslen-derived "
       "size and no terminator element, followed by a same-source strcpy/"
       "stpcpy/wcscpy/strcat/wcscat or sprintf(\"%s\"). Byte/character scaling "
       "uses the target pointee type. Integer casts and unmodified local "
       "spills are followed; other consumers and cross-call flow remain "
       "unknown."},
      {"cpp/constant-array-overflow", "error",
       "experimental/Security/CWE/CWE-193/ConstantSizeArrayOffByOne.ql",
       "Retained loads/stores with constant out-of-bounds byte offsets from "
       "static stack/global arrays with more than one element. Follows local "
       "spills and typed GEP/casts; dynamic range and interprocedural sources "
       "are not yet modeled."},
      {"cpp/improper-null-termination", "warning",
       "Likely Bugs/Memory Management/ImproperNullTermination.ql",
       "Tracked local-array/field contents immediately before string "
       "consumers, "
       "using MemorySSA stores, zero regions, memset/memcpy, string copies/"
       "concatenation, formatting, readlink and exact pointer aliases. CFG "
       "joins "
       "require matching termination state; mixed/unknown/escaped/loop states "
       "remain unknown. Direct null-consuming wrappers are summarized."},
      {"cpp/user-controlled-null-termination-tainted", "warning",
       "Security/CWE/CWE-170/ImproperNullTerminationTainted.ql",
       "Known full-buffer read/recv/recvfrom/fread overwrites reaching modeled "
       "string consumers without an established terminator. Uses PDG memory "
       "taint at the consumer, preserving clean overwrites and field offsets. "
       "Partial/unknown write bounds and recvmsg objects remain unknown."},
      {"cpp/overflowing-snprintf", "warning",
       "Likely Bugs/Format/SnprintfOverflow.ql",
       "snprintf/vsnprintf/snprintf_s size expressions derived through "
       "arithmetic from "
       "snprintf's full return length, including SSA/MemorySSA loop backedges, "
       "pointer updates and casts. Controlling relational/equality guards cut "
       "flow. Unknown aliases, call summaries and arbitrary range proofs "
       "remain partial."},
      {"cpp/overflow-calculated", "warning", "Critical/OverflowCalculated.ql",
       "A known strlen/wcslen-affine allocation followed by same-object source "
       "copy and append needing more than the reserved constant slack. Tracks "
       "real memory versions and target character width; sums of unrelated "
       "lengths, custom allocators and cross-call content remain unknown."},
      {"cpp/overflow-destination", "warning", "Critical/OverflowDestination.ql",
       "PDG input-content flow into memcpy/memmove/strncpy/strncat whose "
       "length "
       "is strlen/wcslen-derived from that source, without a controlling "
       "bound. "
       "sizeof provenance, source AST exclusions and destination-dependent "
       "expressions are not inferred from constant byte counts."},
      {"cpp/unbounded-write", "error", "Security/CWE/CWE-120/UnboundedWrite.ql",
       "PDG input content reaches unbounded strcpy/strcat/sprintf roles with "
       "no "
       "known content/output upper bound; gets and unbounded scanf %s model "
       "implicit input. Precisions, bounded copies, literals and clean "
       "overwrite "
       "facts suppress. Full CodeQL barrier and C++ library models remain "
       "partial."},
      {"cpp/overrunning-write", "error", "Security/CWE/CWE-120/OverrunWrite.ql",
       "Unbounded printf-family string writes with known destination capacity "
       "and format/type-derived maximum output beyond it. Parses width, "
       "precision, positional/star arguments and target integer/pointer "
       "widths. "
       "Unknown directives, locale grouping, va_list and long double stay "
       "unknown."},
      {"cpp/very-likely-overrunning-write", "error",
       "Security/CWE/CWE-120/VeryLikelyOverrunWrite.ql",
       "Known literal/content/range-derived lengths for strcpy/strcat/sprintf "
       "and scanf %s field widths exceed object capacity including the "
       "terminator. "
       "MemorySSA content updates and integer Guard/SCEV ranges are shared. "
       "Cross-call sources, indirect calls and C++ string objects are "
       "excluded."},
      {"cpp/overrunning-write-with-float", "error",
       "Security/CWE/CWE-120/OverrunWriteFloat.ql",
       "Printf writes whose full %f upper bound exceeds capacity while the "
       "CodeQL eight-character %f estimate fits; supports "
       "literal/constant-star "
       "widths and precisions and other modeled conversions. Va_lists, long "
       "double and locale-specific conversions stay unknown."},
      {"cpp/badly-bounded-write", "error",
       "Security/CWE/CWE-120/BadlyBoundedWrite.ql",
       "Known explicit limits of strncpy/strncat/fgets/snprintf and their "
       "modeled "
       "wide variants exceed remaining destination capacity. Correct byte/"
       "character units and signed fgets counts are preserved; unknown dynamic "
       "limits and source configuration-file exclusions are unavailable."},
      {"cpp/potential-buffer-overflow", "warning",
       "Likely Bugs/Memory Management/PotentialBufferOverflow.ql",
       "Deprecated upstream rule: sprintf-family format-output maximum exceeds "
       "known destination capacity. Reuses shared format/content/range bounds, "
       "including %f extrema; va_list and custom format declarations remain "
       "unknown."},
      {"cpp/unclear-array-index-validation", "warning",
       "Security/CWE/CWE-129/ImproperArrayIndexValidation.ql",
       "PDG scalar user-input origins reach retained indexed load/store GEPs "
       "without a controlling range/equality guard or proven fixed-array "
       "range. "
       "Uses signedness/nonnegativity and shared LLVM range facts; "
       "source-level "
       "variable/macro exclusions, indirect calls and full Guard models are "
       "partial."}};
  return rules;
}

bool BoundsAnalysis::requiresTaint(const std::string &id) {
  return id == "cpp/unbounded-write" || id == "cpp/overflow-destination" ||
         id == "cpp/unclear-array-index-validation" ||
         id == "cpp/user-controlled-null-termination-tainted";
}

BoundsAnalysisResult BoundsAnalysis::analyze(const Module &module,
                                      const TaintFlowResult *taint) const {
  BoundsAnalysis query(const_cast<Module &>(module));
  return {query.impl_->analyze(taint)};
}

} // namespace pdg
