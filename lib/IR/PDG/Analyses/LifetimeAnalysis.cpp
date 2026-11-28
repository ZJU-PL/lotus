#include "IR/PDG/Analyses/LifetimeAnalysis.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Models/ValueFacts.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>

using namespace llvm;

namespace pdg {
namespace {

enum class Family { Memory, File, Descriptor, Mutex, Semaphore };
enum class Identity { None, Base, Interior };

struct Object {
  const Value *origin;
  const Instruction *acquire;
  Family family;
};

struct Slot {
  const AllocaInst *base = nullptr;
  int64_t offset = 0;
  bool operator<(const Slot &other) const {
    if (base != other.base)
      return std::less<const AllocaInst *>()(base, other.base);
    return offset < other.offset;
  }
};

struct Comparison {
  const Value *left;
  const Value *right;
  ICmpInst::Predicate predicate;
  bool operator<(const Comparison &other) const {
    if (left != other.left)
      return std::less<const Value *>()(left, other.left);
    if (right != other.right)
      return std::less<const Value *>()(right, other.right);
    return predicate < other.predicate;
  }
};

struct LockLocation {
  const Value *base;
  int64_t offset;
  bool operator<(const LockLocation &other) const {
    if (base != other.base)
      return std::less<const Value *>()(base, other.base);
    return offset < other.offset;
  }
  bool operator==(const LockLocation &other) const {
    return base == other.base && offset == other.offset;
  }
};

struct LockModel {
  Family family;
  bool acquire;
  int argument = 0;
  const char *global_lock = nullptr;
  /// A successful lock call may return zero (pthread/VxWorks) or true
  /// (std::mutex::try_lock). Void APIs have no failure return alternative.
  unsigned success = 0;
  bool try_lock = false;
  bool recursive = false;
};

struct State {
  bool acquired = false;
  const Instruction *released = nullptr;
  const Instruction *release_detail = nullptr;
  bool escaped = false;
  bool success_checked = false;
  std::map<const Value *, Identity> aliases;
  std::map<Slot, Identity> slots;
  std::map<Slot, Identity> owners;
  std::map<const Value *, const Value *> scalar_roots;
  std::map<Slot, const Value *> scalar_slots;
  std::map<const Value *, bool> conditions;
  std::map<const Value *, const ConstantInt *> integer_equal;
  std::map<const Value *, std::set<const ConstantInt *>> integer_excluded;
  std::map<Comparison, bool> comparisons;
  std::set<const AllocaInst *> escaped_locals;
  std::set<const Value *> published_pointers;
  std::set<const Value *> nonnull_values;
  std::map<const CallBase *, Slot> failed_realloc_slots;
  std::set<const CallBase *> overwritten_reallocs;
  std::map<const Value *, const CallBase *> failed_results;
  std::map<Slot, const CallBase *> failed_result_slots;
  /// Net acquisitions relative to function entry; an initial unlock can
  /// consume a caller-owned hold. Clamping negative counts would falsely
  /// report an unlock-then-lock helper that restores its input state.
  std::map<LockLocation, int> lock_counts;
  Optional<LockLocation> acquired_lock;
  bool jpl_unmatched = false;
};

struct ReturnFrame {
  const CallInst *call;
  const BasicBlock *block;
  const BasicBlock *predecessor;
  const Instruction *resume;
};

struct ReleaseEffect {
  Family family;
  unsigned argument;
  const Instruction *site;
};

struct SimpleSummary {
  std::vector<ReleaseEffect> releases;
  const Value *returned = nullptr;
};

using SummaryCache = std::map<const Function *, Optional<SimpleSummary>>;

struct Work {
  const BasicBlock *block;
  const BasicBlock *predecessor;
  State state;
  const Instruction *resume = nullptr;
  std::vector<ReturnFrame> frames;
};

bool named(const Function &function, StringRef name) {
  return function.isDeclaration() &&
         ValueFacts::hasLibraryName(function, name.str(), true);
}

std::string fastDemangle(StringRef raw) {
  if (!raw.startswith("_Z") && !raw.startswith("___Z"))
    return raw.str();
  return llvm::demangle(raw.str());
}

Optional<LockModel> lockModel(const CallBase &call) {
  const Function *callee = ValueFacts::callee(call);
  if (!callee)
    return None;
  if (callee->isDeclaration()) {
    for (StringRef name : {"pthread_mutex_lock", "pthread_mutex_trylock",
                           "mtx_lock", "mtx_trylock"})
      if (ValueFacts::hasLibraryName(*callee, name.str(), true) &&
          !call.arg_empty()) {
        LockModel model{Family::Mutex, true};
        model.try_lock = name.endswith("trylock");
        return model;
      }
    for (StringRef name : {"pthread_mutex_unlock", "mtx_unlock"})
      if (ValueFacts::hasLibraryName(*callee, name.str(), true) &&
          !call.arg_empty())
        return LockModel{Family::Mutex, false};
    if (ValueFacts::hasLibraryName(*callee, "semTake", true) &&
        !call.arg_empty())
      return LockModel{Family::Semaphore, true};
    if (ValueFacts::hasLibraryName(*callee, "semGive", true) &&
        !call.arg_empty())
      return LockModel{Family::Semaphore, false};
    for (const auto &names :
         {std::pair<const char *, const char *>{"taskLock", "taskUnlock"},
          {"intLock", "intUnlock"},
          {"taskRtpLock", "taskRtpUnlock"}}) {
      if (ValueFacts::hasLibraryName(*callee, names.first, true))
        return LockModel{Family::Semaphore, true, -1, names.first};
      if (ValueFacts::hasLibraryName(*callee, names.second, true))
        return LockModel{Family::Semaphore, false, -1, names.first};
    }
  }
  StringRef raw = callee->getName();
  if (!call.arg_empty() && (raw.startswith("_Z") || raw.startswith("___Z"))) {
    const std::string full = llvm::demangle(raw.str());
    const std::string base = ValueFacts::functionBaseName(*callee);
    for (StringRef type :
         {"std::mutex", "std::recursive_mutex", "std::timed_mutex",
          "std::recursive_timed_mutex", "std::__1::mutex",
          "std::__1::recursive_mutex", "std::__1::timed_mutex",
          "std::__1::recursive_timed_mutex"})
      if (StringRef(full).startswith((type + "::" + base + "(").str())) {
        if (base == "lock" || base == "try_lock") {
          LockModel model{Family::Mutex, true, 0, nullptr,
                          base == "try_lock" ? 1u : 0u};
          model.try_lock = base == "try_lock";
          model.recursive = type.contains("recursive");
          return model;
        }
        if (base == "unlock")
          return LockModel{Family::Mutex, false};
      }
  }
  return None;
}

const Value *lockValue(const CallBase &call, const LockModel &model) {
  if (model.argument >= 0)
    return call.getArgOperand(static_cast<unsigned>(model.argument));
  if (model.acquire)
    return ValueFacts::callee(call);
  if (const Function *function =
          call.getModule()->getFunction(model.global_lock))
    return function;
  for (const Function &function : *call.getModule())
    if (named(function, model.global_lock))
      return &function;
  return nullptr;
}

Optional<Family> acquireFamily(const CallBase &call) {
  const Function *callee = ValueFacts::callee(call);
  if (!callee || !callee->isDeclaration())
    return None;
  // realloc has success-dependent effects on both old and new objects, and is
  // deliberately an ownership barrier until a relational model is available.
  if (LibraryModels::allocation(*callee) != AllocationKind::Unknown &&
      !named(*callee, "realloc"))
    return Family::Memory;
  for (StringRef name :
       {"fopen", "fopen64", "fdopen", "tmpfile", "tmpfile64", "popen"})
    if (named(*callee, name))
      return Family::File;
  for (StringRef name : {"open", "open64", "creat", "creat64", "socket",
                         "accept", "accept4", "dup"})
    if (named(*callee, name))
      return Family::Descriptor;
  return None;
}

bool releases(const CallBase &call, Family family) {
  const Function *callee = ValueFacts::callee(call);
  if (!callee || !callee->isDeclaration() || call.arg_empty())
    return false;
  if (family == Family::Memory)
    return LibraryModels::release(*callee) != ReleaseKind::Unknown;
  if (family == Family::File)
    return named(*callee, "fclose") || named(*callee, "pclose");
  if (family == Family::Descriptor)
    return named(*callee, "close");
  auto model = lockModel(call);
  return model && model->family == family && !model->acquire;
}

bool reachesRelease(const Function &function, Family family,
                    std::set<const Function *> &visited,
                    std::map<std::pair<const Function *, Family>, bool> &memo) {
  auto cached = memo.find({&function, family});
  if (cached != memo.end())
    return cached->second;
  if (!visited.insert(&function).second || function.isDeclaration())
    return false;
  for (const BasicBlock &block : function)
    for (const Instruction &instruction : block)
      if (const auto *call = dyn_cast<CallBase>(&instruction)) {
        if (releases(*call, family)) {
          memo[{&function, family}] = true;
          return true;
        }
        if (const Function *callee = ValueFacts::callee(*call))
          if (reachesRelease(*callee, family, visited, memo)) {
            memo[{&function, family}] = true;
            return true;
          }
      }
  if (visited.size() == 1 || &function == *visited.begin())
    memo[{&function, family}] = false;
  return false;
}

bool reachesMutexLock(const Function &function,
                      std::set<const Function *> &visited,
                      std::map<const Function *, bool> &memo) {
  auto cached = memo.find(&function);
  if (cached != memo.end())
    return cached->second;
  if (!visited.insert(&function).second || function.isDeclaration())
    return false;
  for (const BasicBlock &block : function)
    for (const Instruction &instruction : block)
      if (const auto *call = dyn_cast<CallBase>(&instruction)) {
        if (auto model = lockModel(*call))
          if (model->family == Family::Mutex && model->acquire) {
            memo[&function] = true;
            return true;
          }
        if (const Function *callee = ValueFacts::callee(*call))
          if (reachesMutexLock(*callee, visited, memo)) {
            memo[&function] = true;
            return true;
          }
      }
  return false;
}

/// These slots are exact addresses, rather than may-alias root objects.
Optional<Slot> localSlot(const Value *pointer, const DataLayout &layout) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return None;
  int64_t offset = 0;
  const Value *base = GetPointerBaseWithConstantOffset(pointer, offset, layout);
  const auto *allocation = dyn_cast<AllocaInst>(base);
  if (!allocation)
    return None;
  return Slot{allocation, offset};
}

Identity identity(const Value *value, const State &state) {
  if (!value)
    return Identity::None;
  auto found = state.aliases.find(value);
  if (found != state.aliases.end())
    return found->second;
  if (const auto *cast = dyn_cast<Operator>(value)) {
    if (cast->getOpcode() == Instruction::BitCast ||
        cast->getOpcode() == Instruction::AddrSpaceCast)
      return identity(cast->getOperand(0), state);
    if (cast->getOpcode() == Instruction::GetElementPtr) {
      Identity base = identity(cast->getOperand(0), state);
      if (base == Identity::None)
        return base;
      const auto *gep = llvm::cast<GEPOperator>(cast);
      return gep->hasAllZeroIndices() ? base : Identity::Interior;
    }
  }
  return Identity::None;
}

void bind(State &state, const Value *value, Identity id) {
  if (id == Identity::None)
    state.aliases.erase(value);
  else
    state.aliases[value] = id;
}

/// An address passed to an unknown call can access any member of its local
/// aggregate. Byte-overlapping stores also invalidate aliases, even when the
/// offsets differ (for example union or casted stores).
bool invalidateSlots(State &state, const Slot &location,
                     const DataLayout &layout, uint64_t bytes = 0) {
  bool removed_alias = false;
  std::vector<Slot> invalidated;
  for (const auto &entry : state.scalar_slots) {
    if (entry.first.base != location.base)
      continue;
    Type *type = entry.second->getType();
    uint64_t width =
        type->isSized() ? layout.getTypeStoreSize(type).getKnownMinValue() : 0;
    if (bytes == 0 || width == 0 ||
        (entry.first.offset < location.offset + static_cast<int64_t>(bytes) &&
         location.offset < entry.first.offset + static_cast<int64_t>(width)))
      invalidated.push_back(entry.first);
  }
  for (const Slot &slot : invalidated) {
    removed_alias |= state.slots.erase(slot) != 0;
    state.scalar_slots.erase(slot);
    state.failed_result_slots.erase(slot);
  }
  return removed_alias;
}

// Recognize Boolean identity/inversion. A later test of the same SSA condition
// cannot select the opposite alternative. Conditions with unrelated arithmetic
// remain unknown and are explicitly outside the feasibility guarantee.
const Value *scalarRoot(const Value *value, const State &state) {
  auto found = state.scalar_roots.find(value);
  return found == state.scalar_roots.end() ? value : found->second;
}

void setScalarRoot(State &state, const Value *key, const Value *root) {
  if (root == key)
    state.scalar_roots.erase(key);
  else
    state.scalar_roots[key] = root;
}

const ConstantInt *knownInteger(const Value *value, const State &state) {
  value = scalarRoot(value, state);
  if (const auto *constant = dyn_cast_or_null<ConstantInt>(value))
    return constant;
  auto equality = state.integer_equal.find(value);
  if (equality != state.integer_equal.end())
    return equality->second;
  auto boolean = state.conditions.find(value);
  if (value && value->getType()->isIntegerTy() &&
      boolean != state.conditions.end() && !boolean->second)
    return ConstantInt::get(cast<IntegerType>(value->getType()), 0);
  // Unsigned x <= 0 (or x < 1) establishes zero without a range solver.
  for (const auto &comparison : state.comparisons) {
    const Value *left = comparison.first.left;
    const Value *right = comparison.first.right;
    ICmpInst::Predicate predicate =
        comparison.second
            ? comparison.first.predicate
            : ICmpInst::getInversePredicate(comparison.first.predicate);
    if (scalarRoot(left, state) != value) {
      std::swap(left, right);
      predicate = ICmpInst::getSwappedPredicate(predicate);
    }
    const auto *constant = dyn_cast<ConstantInt>(scalarRoot(right, state));
    if (scalarRoot(left, state) == value && constant &&
        ((predicate == ICmpInst::ICMP_ULE && constant->isZero()) ||
         (predicate == ICmpInst::ICMP_ULT && constant->isOne())))
      return ConstantInt::get(cast<IntegerType>(value->getType()), 0);
  }
  return nullptr;
}

const CallBase *failedResult(const Value *value, const State &state) {
  auto found = state.failed_results.find(value);
  return found == state.failed_results.end() ? nullptr : found->second;
}

LockLocation lockLocation(const Value *value, const DataLayout &layout,
                          const State &state) {
  int64_t offset = 0;
  const Value *base = value;
  for (unsigned depth = 0; base && depth < 16; ++depth) {
    base = scalarRoot(base, state);
    int64_t part = 0;
    if (base->getType()->isPointerTy())
      base = GetPointerBaseWithConstantOffset(base, part, layout);
    offset += part;
    const Value *next = scalarRoot(base, state);
    if (base == next)
      break;
    base = next;
  }
  return {base, offset};
}

Optional<Slot> localSlot(const Value *pointer, const DataLayout &layout,
                         const State &state) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return None;
  int64_t offset = 0;
  const Value *base = GetPointerBaseWithConstantOffset(pointer, offset, layout);
  base = scalarRoot(base, state);
  auto slot = localSlot(base, layout);
  if (slot)
    slot->offset += offset;
  return slot;
}

const AllocaInst *localBase(const Value *pointer, const State &state) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return nullptr;
  const Value *base = scalarRoot(getUnderlyingObject(pointer), state);
  return dyn_cast<AllocaInst>(getUnderlyingObject(base));
}

std::pair<Comparison, bool> comparison(const ICmpInst &compare,
                                       const State &state) {
  const Value *left = scalarRoot(compare.getOperand(0), state);
  const Value *right = scalarRoot(compare.getOperand(1), state);
  auto predicate = compare.getPredicate();
  if (std::less<const Value *>()(right, left)) {
    std::swap(left, right);
    predicate = ICmpInst::getSwappedPredicate(predicate);
  }
  auto inverse = ICmpInst::getInversePredicate(predicate);
  bool invert = inverse < predicate;
  return {{left, right, invert ? inverse : predicate}, invert};
}

std::pair<const Value *, bool> conditionRoot(const Value *value,
                                             const State &state) {
  bool invert = false;
  for (unsigned depth = 0; depth < 32; ++depth) {
    value = scalarRoot(value, state);
    if (const auto *binary = dyn_cast<BinaryOperator>(value)) {
      if (binary->getOpcode() == Instruction::Xor &&
          binary->getType()->isIntegerTy(1)) {
        const auto *constant = dyn_cast<ConstantInt>(binary->getOperand(1));
        if (constant && constant->isOne()) {
          invert = !invert;
          value = binary->getOperand(0);
          continue;
        }
      }
    }
    if (const auto *compare = dyn_cast<ICmpInst>(value)) {
      const Value *tested = compare->getOperand(0);
      const Value *other = compare->getOperand(1);
      if (isa<Constant>(tested))
        std::swap(tested, other);
      const auto *constant = dyn_cast<Constant>(other);
      if (constant && constant->isNullValue() &&
          (compare->getPredicate() == ICmpInst::ICMP_EQ ||
           compare->getPredicate() == ICmpInst::ICMP_NE)) {
        invert ^= compare->getPredicate() == ICmpInst::ICMP_EQ;
        value = tested;
        continue;
      }
    }
    if (const auto *cast = dyn_cast<CastInst>(value))
      if (cast->getOpcode() == Instruction::ZExt ||
          cast->getOpcode() == Instruction::SExt) {
        value = cast->getOperand(0);
        continue;
      }
    break;
  }
  return {value, invert};
}

Optional<bool> condition(const Value *value, const State &state,
                         Family family) {
  if (const auto *constant = dyn_cast<ConstantInt>(value))
    return !constant->isZero();
  auto root = conditionRoot(value, state);
  if (isa<ConstantPointerNull>(root.first))
    return root.second;
  if (state.nonnull_values.count(root.first))
    return !root.second;
  if (const auto *constant = dyn_cast<ConstantInt>(root.first))
    return (!constant->isZero()) != root.second;
  auto found = state.conditions.find(root.first);
  if (found != state.conditions.end())
    return found->second != root.second;
  auto integer = state.integer_equal.find(root.first);
  if (integer != state.integer_equal.end())
    return (!integer->second->isZero()) != root.second;
  if (const auto *compare = dyn_cast<ICmpInst>(root.first)) {
    auto normalized = comparison(*compare, state);
    auto known = state.comparisons.find(normalized.first);
    if (known != state.comparisons.end())
      return known->second != (normalized.second != root.second);
  }
  const auto *compare = dyn_cast<ICmpInst>(value);
  if (!compare)
    return None;
  const auto *left_constant =
      dyn_cast<Constant>(scalarRoot(compare->getOperand(0), state));
  const auto *right_constant =
      dyn_cast<Constant>(scalarRoot(compare->getOperand(1), state));
  if (left_constant && right_constant &&
      left_constant->getType() == right_constant->getType())
    if (const auto *answer = dyn_cast<ConstantInt>(ConstantExpr::getICmp(
            compare->getPredicate(), const_cast<Constant *>(left_constant),
            const_cast<Constant *>(right_constant))))
      return !answer->isZero();
  if (compare->getPredicate() == ICmpInst::ICMP_EQ ||
      compare->getPredicate() == ICmpInst::ICMP_NE) {
    const Value *tested = scalarRoot(compare->getOperand(0), state);
    const auto *constant = dyn_cast<ConstantInt>(compare->getOperand(1));
    if (!constant) {
      tested = scalarRoot(compare->getOperand(1), state);
      constant = dyn_cast<ConstantInt>(compare->getOperand(0));
    }
    if (constant) {
      auto equal = state.integer_equal.find(tested);
      if (equal != state.integer_equal.end())
        return (equal->second == constant) ==
               (compare->getPredicate() == ICmpInst::ICMP_EQ);
      auto excluded = state.integer_excluded.find(tested);
      if (excluded != state.integer_excluded.end() &&
          excluded->second.count(constant))
        return compare->getPredicate() == ICmpInst::ICMP_NE;
    }
  }
  if (!state.acquired)
    return None;
  const Value *tracked = compare->getOperand(0);
  const Value *constant = compare->getOperand(1);
  ICmpInst::Predicate predicate = compare->getPredicate();
  if (identity(tracked, state) == Identity::None) {
    std::swap(tracked, constant);
    predicate = ICmpInst::getSwappedPredicate(predicate);
  }
  if (identity(tracked, state) != Identity::Base)
    return None;
  if (family != Family::Descriptor && isa<ConstantPointerNull>(constant)) {
    if (predicate == ICmpInst::ICMP_EQ)
      return false;
    if (predicate == ICmpInst::ICMP_NE)
      return true;
  }
  if (family == Family::Descriptor) {
    const auto *integer = dyn_cast<ConstantInt>(constant);
    if (!integer)
      return None;
    if (integer->isMinusOne()) {
      if (predicate == ICmpInst::ICMP_EQ || predicate == ICmpInst::ICMP_SLE)
        return false;
      if (predicate == ICmpInst::ICMP_NE || predicate == ICmpInst::ICMP_SGT)
        return true;
    }
    if (integer->getValue().isNegative()) {
      if (predicate == ICmpInst::ICMP_EQ)
        return false;
      if (predicate == ICmpInst::ICMP_NE)
        return true;
    }
    if (integer->isZero()) {
      if (predicate == ICmpInst::ICMP_SLT)
        return false;
      if (predicate == ICmpInst::ICMP_SGE)
        return true;
    }
  }
  return None;
}

bool assumeInteger(State &state, const Value *value,
                   const ConstantInt *constant, bool equal) {
  value = scalarRoot(value, state);
  if (const auto *known = dyn_cast<ConstantInt>(value))
    return (known == constant) == equal;
  auto known = state.integer_equal.find(value);
  if (known != state.integer_equal.end())
    return (known->second == constant) == equal;
  auto excluded = state.integer_excluded.find(value);
  if (equal && excluded != state.integer_excluded.end() &&
      excluded->second.count(constant))
    return false;
  auto boolean = state.conditions.find(value);
  if (equal && boolean != state.conditions.end() &&
      boolean->second == constant->isZero())
    return false;
  if (equal)
    state.integer_equal[value] = constant;
  else
    state.integer_excluded[value].insert(constant);
  return true;
}

bool assume(State &state, const Value *value, bool truth, Family family) {
  auto known = condition(value, state, family);
  if (known && *known != truth)
    return false;
  if (family == Family::Descriptor)
    if (const auto *compare = dyn_cast<ICmpInst>(value)) {
      const Value *tracked = compare->getOperand(0);
      const auto *constant = dyn_cast<ConstantInt>(compare->getOperand(1));
      if (!constant) {
        tracked = compare->getOperand(1);
        constant = dyn_cast<ConstantInt>(compare->getOperand(0));
      }
      if (constant && identity(tracked, state) == Identity::Base && known &&
          (constant->isZero() || constant->isMinusOne()))
        state.success_checked = true;
    }
  auto root = conditionRoot(value, state);
  state.conditions[root.first] = truth != root.second;
  if (const auto *compare = dyn_cast<ICmpInst>(root.first)) {
    auto normalized = comparison(*compare, state);
    bool normalized_truth = truth != (root.second != normalized.second);
    auto known = state.comparisons.find(normalized.first);
    if (known != state.comparisons.end() && known->second != normalized_truth)
      return false;
    state.comparisons[normalized.first] = normalized_truth;
  }
  if (const auto *compare = dyn_cast<ICmpInst>(value)) {
    if (compare->getPredicate() == ICmpInst::ICMP_EQ ||
        compare->getPredicate() == ICmpInst::ICMP_NE) {
      const Value *tested = compare->getOperand(0);
      const auto *constant = dyn_cast<ConstantInt>(compare->getOperand(1));
      if (!constant) {
        tested = compare->getOperand(1);
        constant = dyn_cast<ConstantInt>(compare->getOperand(0));
      }
      if (constant && !assumeInteger(state, tested, constant,
                                     truth == (compare->getPredicate() ==
                                               ICmpInst::ICMP_EQ)))
        return false;
    }
  }
  return true;
}

struct WorkKey {
  std::vector<uint64_t> words;
  size_t hash = 0;

  bool operator==(const WorkKey &other) const {
    return hash == other.hash && words == other.words;
  }
};

struct WorkKeyHash {
  size_t operator()(const WorkKey &k) const noexcept { return k.hash; }
};

WorkKey key(const Work &work) {
  WorkKey k;
  auto &w = k.words;
  w.reserve(64);
  auto ptr = [&](const void *p) {
    w.push_back(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)));
  };
  auto num = [&](uint64_t v) { w.push_back(v); };

  ptr(work.block);
  ptr(work.predecessor);
  ptr(work.state.released);
  ptr(work.resume);
  uint64_t flags = (work.state.acquired ? 1u : 0u) |
                   (work.state.escaped ? 2u : 0u) |
                   (work.state.success_checked ? 4u : 0u) |
                   (work.state.jpl_unmatched ? 8u : 0u) |
                   (work.state.acquired_lock ? 16u : 0u);
  num(flags);
  if (work.state.acquired_lock) {
    ptr(work.state.acquired_lock->base);
    num(static_cast<uint64_t>(work.state.acquired_lock->offset));
  }
  num(work.frames.size());
  for (const ReturnFrame &frame : work.frames) {
    ptr(frame.call);
    ptr(frame.resume);
  }
  num(work.state.aliases.size());
  for (const auto &alias : work.state.aliases) {
    ptr(alias.first);
    num(static_cast<uint64_t>(alias.second));
  }
  num(work.state.slots.size());
  for (const auto &slot : work.state.slots) {
    ptr(slot.first.base);
    num(static_cast<uint64_t>(slot.first.offset));
    num(static_cast<uint64_t>(slot.second));
  }
  num(work.state.owners.size());
  for (const auto &slot : work.state.owners) {
    ptr(slot.first.base);
    num(static_cast<uint64_t>(slot.first.offset));
    num(static_cast<uint64_t>(slot.second));
  }
  num(work.state.scalar_roots.size());
  for (const auto &entry : work.state.scalar_roots) {
    ptr(entry.first);
    ptr(entry.second);
  }
  num(work.state.scalar_slots.size());
  for (const auto &entry : work.state.scalar_slots) {
    ptr(entry.first.base);
    num(static_cast<uint64_t>(entry.first.offset));
    ptr(entry.second);
  }
  num(work.state.conditions.size());
  for (const auto &entry : work.state.conditions) {
    ptr(entry.first);
    num(entry.second ? 1u : 0u);
  }
  num(work.state.integer_equal.size());
  for (const auto &entry : work.state.integer_equal) {
    ptr(entry.first);
    ptr(entry.second);
  }
  num(work.state.integer_excluded.size());
  for (const auto &entry : work.state.integer_excluded) {
    ptr(entry.first);
    num(entry.second.size());
    for (const auto *constant : entry.second)
      ptr(constant);
  }
  num(work.state.comparisons.size());
  for (const auto &entry : work.state.comparisons) {
    ptr(entry.first.left);
    ptr(entry.first.right);
    num((static_cast<uint64_t>(entry.first.predicate) << 1) |
        (entry.second ? 1u : 0u));
  }
  num(work.state.escaped_locals.size());
  for (const AllocaInst *local : work.state.escaped_locals)
    ptr(local);
  num(work.state.published_pointers.size());
  for (const Value *published : work.state.published_pointers)
    ptr(published);
  num(work.state.nonnull_values.size());
  for (const Value *value : work.state.nonnull_values)
    ptr(value);
  num(work.state.failed_realloc_slots.size());
  for (const auto &failure : work.state.failed_realloc_slots) {
    ptr(failure.first);
    ptr(failure.second.base);
    num(static_cast<uint64_t>(failure.second.offset));
  }
  num(work.state.overwritten_reallocs.size());
  for (const CallBase *call : work.state.overwritten_reallocs)
    ptr(call);
  num(work.state.failed_results.size());
  for (const auto &value : work.state.failed_results) {
    ptr(value.first);
    ptr(value.second);
  }
  num(work.state.failed_result_slots.size());
  for (const auto &slot : work.state.failed_result_slots) {
    ptr(slot.first.base);
    num(static_cast<uint64_t>(slot.first.offset));
    ptr(slot.second);
  }
  num(work.state.lock_counts.size());
  for (const auto &count : work.state.lock_counts) {
    ptr(count.first.base);
    num(static_cast<uint64_t>(count.first.offset));
    num(static_cast<uint64_t>(count.second));
  }

  uint64_t h = 14695981039346656037ULL;
  for (uint64_t word : w) {
    h ^= word;
    h *= 1099511628211ULL;
  }
  k.hash = static_cast<size_t>(h);
  return k;
}

bool dereferences(const CallBase &call, unsigned argument) {
  if (const auto *memory = dyn_cast<MemIntrinsic>(&call)) {
    if (const auto *length = dyn_cast<ConstantInt>(memory->getLength()))
      if (length->isZero())
        return false;
    return argument == 0 || (isa<MemTransferInst>(memory) && argument == 1);
  }
  const Function *callee = ValueFacts::callee(call);
  if (!callee || !callee->isDeclaration())
    return false;
  if (LibraryModels::readsOnly(*callee))
    return argument <
           (named(*callee, "strlen") || named(*callee, "wcslen") ? 1u : 2u);
  for (StringRef name : {"memcpy", "memmove", "memcmp", "strcpy", "strncpy",
                         "strcat", "strncat", "strcmp", "strncmp"})
    if (named(*callee, name)) {
      if (call.arg_size() > 2)
        if (const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2)))
          if (length->isZero())
            return false;
      return argument < 2;
    }
  if (named(*callee, "memset")) {
    if (call.arg_size() > 2)
      if (const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2)))
        if (length->isZero())
          return false;
    return argument == 0;
  }
  for (StringRef name : {"puts", "fputs", "sprintf"})
    if (named(*callee, name))
      return argument == 0;
  if (named(*callee, "fgets") || named(*callee, "snprintf")) {
    if (call.arg_size() > 1)
      if (const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(1)))
        if (length->isZero() ||
            (named(*callee, "fgets") && length->getValue().isNegative()))
          return false;
    return argument == 0;
  }
  if (named(*callee, "read") || named(*callee, "write")) {
    if (call.arg_size() > 2)
      if (const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2)))
        if (length->isZero())
          return false;
    return argument == 1;
  }
  return false;
}

bool harmlessCall(const CallBase &call) {
  if (isa<DbgInfoIntrinsic>(call))
    return true;
  if (const auto *intrinsic = dyn_cast<IntrinsicInst>(&call))
    return intrinsic->getIntrinsicID() == Intrinsic::lifetime_start ||
           intrinsic->getIntrinsicID() == Intrinsic::lifetime_end;
  return false;
}

bool terminates(const CallBase &call) {
  if (call.doesNotReturn())
    return true;
  if (const auto *intrinsic = dyn_cast<IntrinsicInst>(&call))
    if (intrinsic->getIntrinsicID() == Intrinsic::trap)
      return true;
  if (const Function *callee = ValueFacts::callee(call))
    for (StringRef name : {"exit", "_exit", "_Exit", "quick_exit", "abort",
                           "__assert_fail", "__assert_rtn"})
      if (named(*callee, name))
        return true;
  return false;
}

Optional<SimpleSummary> summarize(const Function &function,
                                  const DataLayout &layout) {
  if (function.isDeclaration() || function.isVarArg() || function.size() != 1)
    return None;
  SimpleSummary summary;
  State values;
  for (const Argument &argument : function.args())
    values.scalar_roots[&argument] = &argument;
  for (const Instruction &instruction : function.getEntryBlock()) {
    if (isa<AllocaInst>(instruction))
      continue;
    if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
      auto slot = localSlot(store->getPointerOperand(), layout, values);
      if (!slot || store->isVolatile() || store->isAtomic())
        return None;
      values.scalar_slots[*slot] = scalarRoot(store->getValueOperand(), values);
    } else if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
      auto slot = localSlot(load->getPointerOperand(), layout, values);
      if (!slot || load->isVolatile() || load->isAtomic())
        return None;
      auto found = values.scalar_slots.find(*slot);
      if (found == values.scalar_slots.end())
        return None;
      values.scalar_roots[load] = found->second;
    } else if (const auto *cast = dyn_cast<CastInst>(&instruction)) {
      if (cast->getOpcode() != Instruction::BitCast &&
          cast->getOpcode() != Instruction::AddrSpaceCast)
        return None;
      values.scalar_roots[cast] = scalarRoot(cast->getOperand(0), values);
    } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
      if (harmlessCall(*call))
        continue;
      if (terminates(*call) || !isa<CallInst>(call))
        return None;
      Optional<Family> family;
      for (Family candidate :
           {Family::Memory, Family::File, Family::Descriptor})
        if (releases(*call, candidate))
          family = candidate;
      if (!family || call->arg_empty())
        return None;
      const auto *argument =
          dyn_cast<Argument>(scalarRoot(call->getArgOperand(0), values));
      if (!argument || argument->getParent() != &function)
        return None;
      summary.releases.push_back({*family, argument->getArgNo(), call});
    } else if (const auto *ret = dyn_cast<ReturnInst>(&instruction)) {
      summary.returned = scalarRoot(ret->getReturnValue(), values);
      if (summary.returned && !isa<Argument>(summary.returned) &&
          !isa<Constant>(summary.returned))
        return None;
    } else {
      return None;
    }
  }
  return summary;
}

struct Explorer {
  const Function &function;
  const DataLayout &layout;
  const Object &object;
  size_t limit;
  LifetimeAnalysisResult &result;
  SummaryCache &summaries;
  bool any_release = false;
  bool any_escape = false;
  bool any_return = false;
  bool incomplete = false;
  bool budget_limited = false;
  bool call_depth_limited = false;
  bool lock_count_limited = false;
  bool has_unlock_attempt = false;
  const Instruction *current_call = nullptr;
  std::set<const Instruction *> leaking_exits;
  std::set<const Instruction *> checked_leaking_exits;
  std::map<const CallBase *, const Instruction *> realloc_leaking_exits;
  std::set<std::pair<std::string, const Instruction *>> emitted;

  Explorer(const Function &analyzed_function, const DataLayout &data_layout,
           const Object &tracked_object, size_t state_budget,
           LifetimeAnalysisResult &output, SummaryCache &summary_cache)
      : function(analyzed_function), layout(data_layout),
        object(tracked_object), limit(state_budget), result(output),
        summaries(summary_cache) {
    if (object.family == Family::Mutex)
      for (const BasicBlock &block : function)
        for (const Instruction &instruction : block)
          if (const auto *call = dyn_cast<CallBase>(&instruction))
            if (auto model = lockModel(*call))
              has_unlock_attempt |=
                  model->family == Family::Mutex && !model->acquire;
  }

  void release(const Instruction *actual_site, State &state,
               const Instruction *context = nullptr) {
    if (state.escaped)
      return;
    const Instruction *site = actual_site;
    if (actual_site->getFunction() != &function)
      site = current_call ? current_call : context ? context : actual_site;
    bool created = false;
    if (state.released && object.family == Family::Memory)
      created = finding("cpp/double-free", "The same memory is released twice.",
                        site, state.released);
    if (state.released && object.family != Family::Memory)
      created = finding("cpp/double-release",
                        "A second resource release is possible.",
                        state.released, site);
    if (state.released && state.release_detail &&
        state.release_detail != state.released && created)
      result.findings.back().evidence.push_back(state.release_detail);
    if (created && actual_site != site)
      result.findings.back().evidence.push_back(actual_site);
    any_release = true;
    state.released = site;
    state.release_detail = actual_site;
  }

  Identity valueIdentity(const Value *value, const State &state) const {
    Identity found = identity(value, state);
    if (found != Identity::None || !value)
      return found;
    if (object.family == Family::Descriptor && state.acquired)
      if (const auto *constant = dyn_cast<ConstantInt>(value))
        if (const ConstantInt *known = knownInteger(object.origin, state))
          if (known->getValue().zextOrTrunc(
                  std::max(known->getBitWidth(), constant->getBitWidth())) ==
              constant->getValue().zextOrTrunc(
                  std::max(known->getBitWidth(), constant->getBitWidth())))
            return Identity::Base;
    if (!state.acquired_lock)
      return Identity::None;
    if ((object.family == Family::Mutex ||
         object.family == Family::Semaphore) &&
        lockLocation(value, layout, state) == *state.acquired_lock)
      return Identity::Base;
    return Identity::None;
  }

  bool finding(StringRef id, StringRef message, const Instruction *site,
               const Instruction *release = nullptr) {
    if (!emitted.insert({id.str(), site}).second)
      return false;
    AnalysisFinding item{id.str(), site, message.str(), {}};
    if (object.acquire)
      item.evidence.push_back(object.acquire);
    if (release)
      item.evidence.push_back(release);
    result.findings.push_back(std::move(item));
    return true;
  }

  void use(const Value *value, const Instruction &instruction, State &state) {
    if (object.family == Family::Memory && state.released && !state.escaped &&
        valueIdentity(value, state) != Identity::None) {
      const Instruction *site =
          instruction.getFunction() != &function && current_call ? current_call
                                                                 : &instruction;
      bool created =
          finding("cpp/use-after-free", "Memory is accessed after its release.",
                  site, state.released);
      if (state.release_detail && state.release_detail != state.released &&
          created)
        result.findings.back().evidence.push_back(state.release_detail);
      if (created && site != &instruction)
        result.findings.back().evidence.push_back(&instruction);
    }
  }

  void escape(State &state) {
    if (!state.acquired)
      return;
    state.escaped = true;
    any_escape = true;
  }

  /// Address escape is a property of the local location, even before the
  /// tracked heap allocation exists. A later store into a published slot can
  /// expose that allocation to an unknown callback. Follow exact local
  /// pointer-to-pointer slots while the address provenance remains known.
  void escapeLocal(const AllocaInst *local, State &state) {
    if (!state.escaped_locals.insert(local).second)
      return;
    for (const auto &entry : state.scalar_slots) {
      if (entry.first.base != local)
        continue;
      if (state.slots.count(entry.first))
        escape(state);
      if (const AllocaInst *nested = localBase(entry.second, state))
        escapeLocal(nested, state);
    }
  }

  void invalidateLocks(const CallBase &call, State &state) {
    const Function *callee = ValueFacts::callee(call);
    if (harmlessCall(call) || call.doesNotAccessMemory() ||
        call.onlyReadsMemory() ||
        (callee &&
         (LibraryModels::readsOnly(*callee) ||
          LibraryModels::allocation(*callee) != AllocationKind::Unknown ||
          LibraryModels::release(*callee) != ReleaseKind::Unknown)) ||
        acquireFamily(call))
      return;
    // Record address publication even before the matching acquisition. A
    // later opaque callback can access a previously captured mutex argument.
    for (unsigned index = 0; index < call.arg_size(); ++index)
      if (call.getArgOperand(index)->getType()->isPointerTy() &&
          !call.paramHasAttr(index, Attribute::NoCapture))
        state.published_pointers.insert(
            lockLocation(call.getArgOperand(index), layout, state).base);
    for (auto &held : state.lock_counts) {
      bool may_change = isa<GlobalVariable>(held.first.base) ||
                        state.published_pointers.count(held.first.base);
      if (const auto *local = dyn_cast<AllocaInst>(held.first.base))
        may_change |= state.escaped_locals.count(local);
      for (const Use &argument : call.args())
        may_change |=
            lockLocation(argument.get(), layout, state).base == held.first.base;
      if (may_change) {
        held.second = 0;
        if (state.acquired_lock && held.first == *state.acquired_lock)
          escape(state);
      }
    }
  }

  bool instruction(const Instruction &instruction, State &state) {
    if (const auto *allocation = dyn_cast<AllocaInst>(&instruction)) {
      invalidateSlots(state, Slot{allocation, 0}, layout);
      for (auto &count : state.lock_counts)
        if (count.first.base == allocation) {
          if (count.second != 0)
            // Re-execution creates a distinct stack object. Do not merge its
            // lock state with the previous dynamic instance of this alloca.
            incomplete = true;
          count.second = 0;
        }
      return true;
    }
    if (&instruction == object.acquire) {
      if (state.acquired) {
        // A repeated static acquire creates another dynamic object; collapsing
        // these objects would manufacture stale aliases and double frees.
        incomplete = true;
        return false;
      }
      state.acquired = true;
      bind(state, &instruction, Identity::Base);
      return true;
    }
    if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
      use(load->getPointerOperand(), instruction, state);
      auto slot = localSlot(load->getPointerOperand(), layout, state);
      Identity id = Identity::None;
      if (slot) {
        auto found = state.slots.find(*slot);
        if (found != state.slots.end())
          id = found->second;
        auto scalar = state.scalar_slots.find(*slot);
        if (scalar != state.scalar_slots.end())
          setScalarRoot(state, load, scalar->second);
        else
          state.scalar_roots.erase(load);
        auto failed = state.failed_result_slots.find(*slot);
        if (failed != state.failed_result_slots.end())
          state.failed_results[load] = failed->second;
        else
          state.failed_results.erase(load);
      }
      bind(state, load, id);
    } else if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
      use(store->getPointerOperand(), instruction, state);
      Identity id = valueIdentity(store->getValueOperand(), state);
      auto slot = localSlot(store->getPointerOperand(), layout, state);
      if (slot) {
        const CallBase *failed = failedResult(store->getValueOperand(), state);
        auto original = state.failed_realloc_slots.find(failed);
        if (failed && original != state.failed_realloc_slots.end() &&
            original->second.base == slot->base &&
            original->second.offset == slot->offset && state.slots.count(*slot))
          state.overwritten_reallocs.insert(failed);
        invalidateSlots(
            state, *slot, layout,
            layout.getTypeStoreSize(store->getValueOperand()->getType())
                .getKnownMinValue());
        state.scalar_slots[*slot] = scalarRoot(store->getValueOperand(), state);
        if (failed)
          state.failed_result_slots[*slot] = failed;
        if (id == Identity::None)
          state.slots.erase(*slot);
        else
          state.slots[*slot] = id;
        if (state.escaped_locals.count(slot->base)) {
          if (id != Identity::None)
            escape(state);
          if (const AllocaInst *stored_address =
                  localBase(store->getValueOperand(), state))
            escapeLocal(stored_address, state);
        }
      } else if (id != Identity::None) {
        if ((object.family == Family::Mutex ||
             object.family == Family::Semaphore) &&
            store->getValueOperand()->getType()->isPointerTy())
          state.published_pointers.insert(
              lockLocation(store->getValueOperand(), layout, state).base);
        escape(state);
      } else {
        if ((object.family == Family::Mutex ||
             object.family == Family::Semaphore) &&
            store->getValueOperand()->getType()->isPointerTy())
          state.published_pointers.insert(
              lockLocation(store->getValueOperand(), layout, state).base);
        if (const AllocaInst *stored_address =
                localBase(store->getValueOperand(), state))
          escapeLocal(stored_address, state);
        // An unknown local address can overlap one of the precise slots.
        // Invalidate their aliases rather than preserving a stale handle.
        const Value *base = store->getPointerOperand()->stripPointerCasts();
        if (const auto *gep = dyn_cast<GEPOperator>(base))
          base = gep->getPointerOperand()->stripPointerCasts();
        if (const auto *alloca = dyn_cast<AllocaInst>(base))
          if (invalidateSlots(state, Slot{alloca, 0}, layout))
            escape(state);
      }
    } else if (const auto *atomic = dyn_cast<AtomicRMWInst>(&instruction)) {
      use(atomic->getPointerOperand(), instruction, state);
      if (valueIdentity(atomic->getValOperand(), state) != Identity::None)
        escape(state);
      if (auto slot = localSlot(atomic->getPointerOperand(), layout, state))
        state.slots.erase(*slot);
      if (auto slot = localSlot(atomic->getPointerOperand(), layout, state))
        state.scalar_slots.erase(*slot);
    } else if (const auto *atomic = dyn_cast<AtomicCmpXchgInst>(&instruction)) {
      use(atomic->getPointerOperand(), instruction, state);
      if (valueIdentity(atomic->getNewValOperand(), state) != Identity::None)
        escape(state);
      if (auto slot = localSlot(atomic->getPointerOperand(), layout, state))
        state.slots.erase(*slot);
      if (auto slot = localSlot(atomic->getPointerOperand(), layout, state))
        state.scalar_slots.erase(*slot);
    } else if (const auto *select = dyn_cast<SelectInst>(&instruction)) {
      auto known = condition(select->getCondition(), state, object.family);
      Identity left = valueIdentity(select->getTrueValue(), state);
      Identity right = valueIdentity(select->getFalseValue(), state);
      bind(state, select,
           known           ? (*known ? left : right)
           : left == right ? left
                           : Identity::None);
      // Different alternatives require a disjunction to prove ownership.
      // Prevent an unsound leak conclusion if the handle may be selected.
      if (!known && left != right &&
          (left != Identity::None || right != Identity::None))
        escape(state);
      if (!known)
        for (const Value *value :
             {select->getTrueValue(), select->getFalseValue()})
          if (auto slot = localSlot(value, layout, state))
            if (state.slots.count(*slot))
              escape(state);
    } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
      if (harmlessCall(*call))
        return true;
      if (object.family == Family::Memory && !call->arg_empty()) {
        const Function *target = ValueFacts::callee(*call);
        if (target && target->isDeclaration() &&
            (target->getName().startswith("_Z") ||
             target->getName().startswith("___Z"))) {
          const std::string full = fastDemangle(target->getName());
          const bool unique =
              (StringRef(full).startswith("std::unique_ptr<") ||
               StringRef(full).startswith("std::__1::unique_ptr<")) &&
              (StringRef(full).contains("std::default_delete<") ||
               StringRef(full).contains("std::__1::default_delete<"));
          auto owner =
              unique ? localSlot(call->getArgOperand(0), layout, state) : None;
          if (unique && owner) {
            const std::string method = ValueFacts::functionBaseName(*target);
            if (method == "unique_ptr" && call->arg_size() == 2 &&
                !StringRef(full).contains("&&")) {
              state.owners[*owner] =
                  valueIdentity(call->getArgOperand(1), state);
              return true;
            }
            if (method == "~unique_ptr" || method == "reset") {
              if (state.owners[*owner] == Identity::Base)
                release(call, state);
              state.owners[*owner] =
                  method == "reset" && call->arg_size() == 2
                      ? valueIdentity(call->getArgOperand(1), state)
                      : Identity::None;
              return true;
            }
            if ((method == "get" || method == "release") &&
                call->getType()->isPointerTy()) {
              bind(state, call, state.owners[*owner]);
              if (method == "release")
                state.owners[*owner] = Identity::None;
              return true;
            }
          }
        }
      }
      if (object.family == Family::Memory)
        if (auto mutex = lockModel(*call))
          if (mutex->family == Family::Mutex && mutex->argument >= 0) {
            // Mutex operations borrow their backing storage. They access it
            // but neither transfer nor release the enclosing heap object.
            use(call->getArgOperand(mutex->argument), instruction, state);
            return true;
          }
      if (object.family == Family::Mutex ||
          object.family == Family::Semaphore)
        invalidateLocks(*call, state);
      if (object.family == Family::Descriptor && state.released)
        if (auto acquired = acquireFamily(*call))
          if (*acquired == Family::Descriptor)
            // FD numbers can be recycled by a new acquisition. The old
            // integer carrier cannot prove the identity of that new object.
            escape(state);
      if (releases(*call, object.family) &&
          valueIdentity(call->getArgOperand(0), state) == Identity::Base) {
        release(call, state);
      } else {
        for (unsigned index = 0; index < call->arg_size(); ++index) {
          const Value *argument = call->getArgOperand(index);
          if (valueIdentity(argument, state) != Identity::None) {
            if (dereferences(*call, index))
              use(argument, instruction, state);
            else
              escape(state);
          }
          // Passing the local address of a tracked slot can transfer ownership
          // or overwrite it through a pointer-to-pointer parameter.
          if (const AllocaInst *local = localBase(argument, state)) {
            for (auto &owner : state.owners)
              if (owner.first.base == local && owner.second != Identity::None) {
                escape(state);
                owner.second = Identity::None;
              }
            escapeLocal(local, state);
            if (invalidateSlots(state, Slot{local, 0}, layout))
              escape(state);
          }
        }
      }
      if (terminates(*call))
        return false;
    } else if (const auto *ret = dyn_cast<ReturnInst>(&instruction)) {
      if (const AllocaInst *address = localBase(ret->getReturnValue(), state))
        escapeLocal(address, state);
      if (valueIdentity(ret->getReturnValue(), state) != Identity::None &&
          object.family != Family::Semaphore)
        any_return = true, escape(state);
      bool live = !state.released;
      if (object.family == Family::Mutex)
        live =
            state.acquired_lock && state.lock_counts[*state.acquired_lock] > 0;
      if (object.family == Family::Semaphore)
        live = state.jpl_unmatched;
      if (state.acquired && live && !state.escaped) {
        leaking_exits.insert(ret);
        if (state.success_checked)
          checked_leaking_exits.insert(ret);
        for (const CallBase *realloc : state.overwritten_reallocs)
          realloc_leaking_exits[realloc] = ret;
      }
    } else if (const auto *cast = dyn_cast<CastInst>(&instruction)) {
      if (cast->getOpcode() == Instruction::BitCast ||
          cast->getOpcode() == Instruction::AddrSpaceCast) {
        bind(state, cast, valueIdentity(cast->getOperand(0), state));
        setScalarRoot(state, cast, scalarRoot(cast->getOperand(0), state));
        if (const CallBase *failed = failedResult(cast->getOperand(0), state))
          state.failed_results[cast] = failed;
        else
          state.failed_results.erase(cast);
      } else if (const ConstantInt *constant =
                     knownInteger(cast->getOperand(0), state)) {
        if (constant->getType() == cast->getOperand(0)->getType())
          state.scalar_roots[cast] = ConstantExpr::getCast(
              cast->getOpcode(), const_cast<ConstantInt *>(constant),
              cast->getType());
      } else if (object.family == Family::Descriptor &&
                 valueIdentity(cast->getOperand(0), state) == Identity::Base &&
                 object.origin->getType()->isIntegerTy() &&
                 cast->getType()->isIntegerTy() &&
                 (cast->getOpcode() == Instruction::SExt ||
                  cast->getOpcode() == Instruction::ZExt ||
                  (cast->getOpcode() == Instruction::Trunc &&
                   cast->getType()->getIntegerBitWidth() >=
                       object.origin->getType()->getIntegerBitWidth()))) {
        bind(state, cast, Identity::Base);
      } else if (valueIdentity(cast->getOperand(0), state) != Identity::None)
        // ptr/int roundtrips and descriptor width changes need an explicit
        // representation model. They cannot retain a guessed object identity.
        escape(state);
      else if (const AllocaInst *address =
                   localBase(cast->getOperand(0), state))
        escapeLocal(address, state);
    } else if (const auto *gep = dyn_cast<GetElementPtrInst>(&instruction)) {
      state.aliases.erase(gep);
      bind(state, gep, valueIdentity(gep, state));
    }
    return true;
  }

  void run() {
    State initial;
    if (!object.acquire) {
      initial.acquired = true;
      if (object.origin)
        bind(initial, object.origin, Identity::Base);
    }
    std::deque<Work> queue;
    queue.push_back({&function.getEntryBlock(), nullptr, std::move(initial)});
    std::unordered_set<WorkKey, WorkKeyHash> seen;
    while (!queue.empty()) {
      Work work = std::move(queue.front());
      queue.pop_front();
      if (!seen.insert(key(work)).second)
        continue;
      ++result.explored_states;
      if (limit != 0 && seen.size() > limit) {
        incomplete = true;
        budget_limited = true;
        break;
      }
      State &state = work.state;
      current_call = work.frames.empty() ? nullptr : work.frames.front().call;
      // LLVM phi operands are selected simultaneously on the incoming edge.
      if (!work.resume && !work.block->phis().empty()) {
        SmallVector<std::tuple<const PHINode *, Identity, bool, const Value *>,
                    4>
            phi_updates;
        for (const PHINode &phi : work.block->phis()) {
          int index =
              work.predecessor ? phi.getBasicBlockIndex(work.predecessor) : -1;
          Identity id =
              index < 0 ? Identity::None
                        : valueIdentity(phi.getIncomingValue(index), state);
          const Value *root =
              index >= 0 ? scalarRoot(phi.getIncomingValue(index), state)
                         : nullptr;
          phi_updates.push_back({&phi, id, index >= 0, root});
        }
        for (const auto &entry : phi_updates) {
          bind(state, std::get<0>(entry), std::get<1>(entry));
          if (std::get<2>(entry))
            setScalarRoot(state, std::get<0>(entry), std::get<3>(entry));
        }
      }
      bool alive = true;
      bool started = work.resume == nullptr;
      for (const Instruction &item : *work.block) {
        if (!started) {
          started = &item == work.resume;
          if (!started)
            continue;
        }
        // An instruction on a back edge denotes a fresh dynamic value.
        state.conditions.erase(&item);
        state.integer_equal.erase(&item);
        state.integer_excluded.erase(&item);
        state.nonnull_values.erase(&item);
        if (!isa<PHINode>(item)) {
          state.aliases.erase(&item);
          state.scalar_roots.erase(&item);
          state.failed_results.erase(&item);
        }
        for (auto it = state.comparisons.begin();
             it != state.comparisons.end();)
          if (it->first.left == &item || it->first.right == &item)
            it = state.comparisons.erase(it);
          else
            ++it;
        if (const auto *call = dyn_cast<CallInst>(&item)) {
          const Function *callee = ValueFacts::callee(*call);
          if (object.family == Family::Mutex ||
              object.family == Family::Semaphore) {
            auto model = lockModel(*call);
            if (model && model->family == object.family) {
              const Value *resource = lockValue(*call, *model);
              LockLocation location = lockLocation(resource, layout, state);
              if (model->acquire) {
                // Standard nonrecursive mutex lock() is a blocking operation.
                // pthread/mtx mutex attributes may permit recursion, so they
                // need an attribute model before making the same conclusion.
                const std::string name =
                    callee ? fastDemangle(callee->getName()) : "";
                if (model->family == Family::Mutex && !state.escaped &&
                    !model->try_lock && !model->recursive &&
                    StringRef(name).startswith("std::") &&
                    state.lock_counts[location] > 0)
                  finding("cpp/twice-locked",
                          "A nonrecursive mutex may already be locked at this "
                          "blocking acquisition.",
                          current_call && !isa<GlobalVariable>(location.base)
                              ? current_call
                              : call);
                bool can_fail = call->getType()->isIntegerTy() &&
                                !(model->global_lock &&
                                  StringRef(model->global_lock) == "intLock");
                if (can_fail) {
                  State failure = state;
                  failure.scalar_roots.erase(call);
                  if (call->getType()->isIntegerTy(1))
                    failure.scalar_roots[call] = ConstantInt::get(
                        call->getType(), model->success ? 0 : 1);
                  else if (model->family == Family::Semaphore)
                    failure.scalar_roots[call] =
                        ConstantInt::getSigned(call->getType(), -1);
                  else {
                    // Failure is any nonzero API error, not a guessed single
                    // error number. Equality tests retain their alternatives.
                    failure.conditions[call] = true;
                    failure.integer_excluded[call].insert(ConstantInt::get(
                        cast<IntegerType>(call->getType()), 0));
                  }
                  queue.push_back({work.block, work.predecessor,
                                   std::move(failure), call->getNextNode(),
                                   work.frames});
                  state.scalar_roots[call] =
                      ConstantInt::get(call->getType(), model->success);
                }
                if (++state.lock_counts[location] > 8) {
                  incomplete = true;
                  lock_count_limited = true;
                  alive = false;
                  break;
                }
                if (call == object.acquire) {
                  state.acquired = true;
                  state.acquired_lock = location;
                  state.jpl_unmatched = true;
                  bind(state, resource, Identity::Base);
                }
              } else {
                if (--state.lock_counts[location] < -8) {
                  incomplete = true;
                  lock_count_limited = true;
                  alive = false;
                  break;
                }
                if (state.acquired_lock && location == *state.acquired_lock) {
                  any_release = true;
                  if (call->getFunction() == &function)
                    state.jpl_unmatched = false;
                }
              }
              continue;
            }
          }
          if (object.family == Family::Descriptor && callee &&
              named(*callee, "close") && !call->arg_empty() &&
              call->getType()->isIntegerTy() &&
              valueIdentity(call->getArgOperand(0), state) == Identity::Base) {
            State failure = state;
            failure.scalar_roots[call] =
                ConstantInt::getSigned(call->getType(), -1);
            // Portable close-error semantics do not establish whether the fd
            // remains owned (notably EINTR). A retry on this alternative is
            // not treated as a proved second release.
            escape(failure);
            queue.push_back({work.block, work.predecessor, std::move(failure),
                             call->getNextNode(), work.frames});
            state.scalar_roots[call] = ConstantInt::get(call->getType(), 0);
            release(call, state);
            continue;
          }
          if (callee && !callee->isDeclaration() && !callee->isVarArg()) {
            auto summary = summaries.find(callee);
            if (summary == summaries.end()) {
              ++result.summary_cache_misses;
              summary =
                  summaries.insert({callee, summarize(*callee, layout)}).first;
            } else {
              ++result.summary_cache_hits;
            }
            if (summary->second) {
              for (const ReleaseEffect &effect : summary->second->releases)
                if (effect.family == object.family &&
                    valueIdentity(call->getArgOperand(effect.argument),
                                  state) == Identity::Base)
                  release(effect.site, state, call);
              const Value *returned = summary->second->returned;
              if (const auto *argument = dyn_cast_or_null<Argument>(returned))
                returned = call->getArgOperand(argument->getArgNo());
              bind(state, call, valueIdentity(returned, state));
              if (returned)
                setScalarRoot(state, call, scalarRoot(returned, state));
              ++result.matched_calls;
              continue;
            }
            bool recursive = callee == work.block->getParent();
            for (const ReturnFrame &frame : work.frames)
              recursive |= callee == frame.block->getParent();
            if (recursive || work.frames.size() >= 16) {
              incomplete = true;
              call_depth_limited = true;
              escape(state);
            } else {
              // A direct, matched call preserves conditional release and
              // ownership alternatives rather than merging a may-release
              // summary into an unconditional free. Reset callee stack/SSA
              // locations so two calls never reuse a previous invocation.
              for (auto it = state.slots.begin(); it != state.slots.end();)
                if (it->first.base->getFunction() == callee)
                  it = state.slots.erase(it);
                else
                  ++it;
              for (auto it = state.scalar_slots.begin();
                   it != state.scalar_slots.end();)
                if (it->first.base->getFunction() == callee)
                  it = state.scalar_slots.erase(it);
                else
                  ++it;
              for (auto it = state.escaped_locals.begin();
                   it != state.escaped_locals.end();)
                if ((*it)->getFunction() == callee)
                  it = state.escaped_locals.erase(it);
                else
                  ++it;
              for (auto it = state.failed_result_slots.begin();
                   it != state.failed_result_slots.end();)
                if (it->first.base->getFunction() == callee)
                  it = state.failed_result_slots.erase(it);
                else
                  ++it;
              auto belongs_to_callee = [&](const Value *v) {
                if (const auto *inst = dyn_cast_or_null<Instruction>(v))
                  return inst->getFunction() == callee;
                if (const auto *arg = dyn_cast_or_null<Argument>(v))
                  return arg->getParent() == callee;
                return false;
              };
              auto erase_callee_entries = [&](auto &map) {
                for (auto it = map.begin(); it != map.end();)
                  if (belongs_to_callee(it->first))
                    it = map.erase(it);
                  else
                    ++it;
              };
              erase_callee_entries(state.aliases);
              erase_callee_entries(state.scalar_roots);
              erase_callee_entries(state.conditions);
              erase_callee_entries(state.integer_equal);
              erase_callee_entries(state.integer_excluded);
              erase_callee_entries(state.failed_results);
              for (auto it = state.nonnull_values.begin();
                   it != state.nonnull_values.end();)
                if (belongs_to_callee(*it))
                  it = state.nonnull_values.erase(it);
                else
                  ++it;
              for (auto it = state.comparisons.begin();
                   it != state.comparisons.end();)
                if (belongs_to_callee(it->first.left) ||
                    belongs_to_callee(it->first.right))
                  it = state.comparisons.erase(it);
                else
                  ++it;
              for (const Argument &formal : callee->args()) {
                const Value *actual = call->getArgOperand(formal.getArgNo());
                bind(state, &formal, valueIdentity(actual, state));
                setScalarRoot(state, &formal, scalarRoot(actual, state));
              }
              auto frames = std::move(work.frames);
              frames.push_back(
                  {call, work.block, work.predecessor, call->getNextNode()});
              ++result.matched_calls;
              queue.push_back({&callee->getEntryBlock(), nullptr,
                               std::move(state), nullptr, std::move(frames)});
              alive = false;
              break;
            }
          }
          if (object.family == Family::Memory && callee &&
              named(*callee, "realloc") && call->arg_size() >= 2 &&
              valueIdentity(call->getArgOperand(0), state) == Identity::Base) {
            const Value *size_value = scalarRoot(call->getArgOperand(1), state);
            const ConstantInt *size = knownInteger(size_value, state);
            use(call->getArgOperand(0), *call, state);
            if (!size || !size->isZero()) {
              if (!size && size_value->getType()->isIntegerTy()) {
                const auto *zero = ConstantInt::get(
                    cast<IntegerType>(size_value->getType()), 0);
                State zero_size = state;
                if (assumeInteger(zero_size, size_value, zero, true)) {
                  escape(zero_size);
                  queue.push_back({work.block, work.predecessor,
                                   std::move(zero_size), call->getNextNode(),
                                   work.frames});
                }
                if (!assumeInteger(state, size_value, zero, false)) {
                  escape(state);
                  continue;
                }
              }
              State failure = state;
              failure.failed_results[call] = call;
              failure.scalar_roots[call] =
                  ConstantPointerNull::get(cast<PointerType>(call->getType()));
              const auto *old_load = dyn_cast<LoadInst>(
                  call->getArgOperand(0)->stripPointerCasts());
              if (old_load)
                if (auto slot =
                        localSlot(old_load->getPointerOperand(), layout, state))
                  failure.failed_realloc_slots[call] = *slot;
              queue.push_back({work.block, work.predecessor, std::move(failure),
                               call->getNextNode(), work.frames});
              state.scalar_roots.erase(call);
              state.failed_results.erase(call);
              state.nonnull_values.insert(call);
              state.released = call;
              any_release = true;
              continue;
            }
          }
        }
        if ((object.family == Family::Mutex ||
             object.family == Family::Semaphore) &&
            isa<InvokeInst>(item) && lockModel(cast<CallBase>(item))) {
          // Invoke normal/unwind lock effects need a separate exception model.
          // Do not route them through ordinary resource release semantics.
          incomplete = true;
          escape(state);
          continue;
        }
        if (const auto *ret = dyn_cast<ReturnInst>(&item)) {
          if (!work.frames.empty()) {
            ReturnFrame frame = work.frames.back();
            auto frames = work.frames;
            frames.pop_back();
            bind(state, frame.call,
                 valueIdentity(ret->getReturnValue(), state));
            if (ret->getReturnValue()) {
              const Value *returned = scalarRoot(ret->getReturnValue(), state);
              const auto *instruction = dyn_cast<Instruction>(returned);
              // A callee-computed scalar denotes a new dynamic result on each
              // invocation; only exact forwarded arguments/constants retain
              // their caller identity across repeated calls.
              if (!instruction ||
                  instruction->getFunction() != ret->getFunction())
                state.scalar_roots[frame.call] = returned;
              else
                state.scalar_roots.erase(frame.call);
            }
            queue.push_back({frame.block, frame.predecessor, std::move(state),
                             frame.resume, std::move(frames)});
            alive = false;
            break;
          }
        }
        if (!isa<PHINode>(item) && !instruction(item, state)) {
          alive = false;
          break;
        }
      }
      if (!alive)
        continue;
      const Instruction *terminator = work.block->getTerminator();
      if (const auto *branch = dyn_cast<BranchInst>(terminator)) {
        if (!branch->isConditional()) {
          queue.push_back({branch->getSuccessor(0), work.block,
                           std::move(state), nullptr, std::move(work.frames)});
        } else {
          State true_state = state;
          if (assume(true_state, branch->getCondition(), true, object.family))
            queue.push_back({branch->getSuccessor(0), work.block,
                             std::move(true_state), nullptr, work.frames});
          if (assume(state, branch->getCondition(), false, object.family))
            queue.push_back({branch->getSuccessor(1), work.block,
                             std::move(state), nullptr,
                             std::move(work.frames)});
        }
      } else if (const auto *dispatch = dyn_cast<SwitchInst>(terminator)) {
        State fallback = state;
        bool default_possible = true;
        for (const auto &entry : dispatch->cases()) {
          State alternative = state;
          if (assumeInteger(alternative, dispatch->getCondition(),
                            entry.getCaseValue(), true))
            queue.push_back({entry.getCaseSuccessor(), work.block,
                             std::move(alternative), nullptr, work.frames});
          default_possible &= assumeInteger(fallback, dispatch->getCondition(),
                                            entry.getCaseValue(), false);
        }
        if (default_possible)
          queue.push_back({dispatch->getDefaultDest(), work.block,
                           std::move(fallback), nullptr,
                           std::move(work.frames)});
      } else {
        SmallVector<const BasicBlock *, 4> succs(successors(work.block));
        for (size_t idx = 0; idx < succs.size(); ++idx) {
          if (idx + 1 == succs.size())
            queue.push_back({succs[idx], work.block, std::move(state), nullptr,
                             std::move(work.frames)});
          else
            queue.push_back(
                {succs[idx], work.block, state, nullptr, work.frames});
        }
      }
    }
    if (incomplete) {
      ++result.incomplete_objects;
      if (budget_limited) {
        ++result.budget_limited_objects;
        result.state_limit_hit = true;
      }
      if (call_depth_limited)
        ++result.call_depth_limited_objects;
      if (lock_count_limited)
        ++result.lock_count_limited_objects;
      return;
    }
    for (const auto &failure : realloc_leaking_exits)
      finding("cpp/memory-leak-on-failed-call-to-realloc",
              "The failed realloc overwrites the original memory handle.",
              failure.first, failure.second);
    if (!object.acquire || leaking_exits.empty())
      return;
    if (object.family == Family::Mutex || object.family == Family::Semaphore) {
      if (object.family == Family::Semaphore || any_release ||
          has_unlock_attempt) {
        finding(object.family == Family::Mutex
                    ? "cpp/unreleased-lock"
                    : "cpp/jpl-c/release-locks-when-acquired",
                "A successful lock remains unmatched on a returning CFG path.",
                object.acquire, *leaking_exits.begin());
      }
      return;
    }
    StringRef never_id =
        object.family == Family::Memory ? "cpp/memory-never-freed"
        : object.family == Family::File ? "cpp/file-never-closed"
                                        : "cpp/descriptor-never-closed";
    if (!any_release && !any_escape) {
      finding(never_id, "This resource is never released before returning.",
              object.acquire);
      result.findings.back().evidence.push_back(*leaking_exits.begin());
    } else if (object.family == Family::Descriptor && any_return) {
      for (const Instruction *exit : checked_leaking_exits)
        finding("cpp/descriptor-may-not-be-closed",
                "The successfully opened descriptor is neither closed nor "
                "returned.",
                exit);
    } else if ((any_release || any_escape) &&
               object.family != Family::Descriptor) {
      StringRef may_id = object.family == Family::Memory
                             ? "cpp/memory-may-not-be-freed"
                             : "cpp/file-may-not-be-closed";
      finding(may_id, "This resource is not released on a returning CFG path.",
              object.acquire);
      result.findings.back().evidence.push_back(*leaking_exits.begin());
    }
  }
};

} // namespace

const std::vector<AnalysisRuleDescriptor> &LifetimeAnalysis::catalog() {
  static const std::string coverage =
      "CFG lifecycle with SSA/phi and exact local stack aliases; cached "
      "straight-line release/return summaries and matched nonrecursive direct "
      "calls (16-frame cap). Known C allocation/release and heap dereferences, "
      "including nonzero realloc success/failure; unknown ownership effects "
      "escape. Local std::unique_ptr with std::default_delete models "
      "raw-pointer "
      "construction, get/release/reset and destructor calls, including invoke "
      "cleanup edges; moves and custom deleters remain excluded. "
      "Preserves Boolean/complementary comparison, equality/switch and "
      "allocation-failure alternatives. General relational feasibility, "
      "indirect calls, heap/global-field aliases, opaque C++ library ownership "
      "and zero-size realloc remain excluded. Budget, recursion and repeated "
      "dynamic-acquisition limits withhold leak claims.";
  static const std::vector<AnalysisRuleDescriptor> rules = {
      {"cpp/double-free", "warning", "Critical/DoubleFree.ql", coverage},
      {"cpp/use-after-free", "warning", "Critical/UseAfterFree.ql", coverage},
      {"cpp/memory-never-freed", "warning", "Critical/MemoryNeverFreed.ql",
       coverage},
      {"cpp/memory-may-not-be-freed", "warning",
       "Critical/MemoryMayNotBeFreed.ql", coverage},
      {"cpp/file-never-closed", "warning", "Critical/FileNeverClosed.ql",
       coverage},
      {"cpp/file-may-not-be-closed", "warning",
       "Critical/FileMayNotBeClosed.ql", coverage},
      {"cpp/descriptor-never-closed", "warning",
       "Critical/DescriptorNeverClosed.ql", coverage},
      {"cpp/descriptor-may-not-be-closed", "warning",
       "Critical/DescriptorMayNotBeClosed.ql",
       "Known descriptor acquisition, an explicit success guard, and "
       "alternative "
       "paths returning the descriptor or exiting without close/return. The "
       "unclosed exit is the finding site; general relational feasibility "
       "excluded."},
      {"cpp/double-release", "warning",
       "experimental/Security/CWE/CWE-675/DoubleRelease.ql",
       "Sequential fclose/pclose/close on the same unchanged object identity; "
       "mutually exclusive CFG alternatives and overwrites remain separate. "
       "The first release is reported; Windows handles and directories "
       "excluded."},
      {"cpp/memory-leak-on-failed-call-to-realloc", "warning",
       "experimental/Security/CWE/CWE-401/MemoryLeakOnFailedCallToRealloc.ql",
       "Success/failure CFG states for nonzero realloc; failure preserves the "
       "old "
       "object and returns null. Reports when an exact local handle is "
       "overwritten "
       "by the failed return and a normal exit neither frees nor transfers the "
       "old object. Zero-size realloc, source assignment reconstruction in "
       "optimized SSA, unknown aliases and heap-field handles excluded."},
      {"cpp/unreleased-lock", "error", "Security/CWE/CWE-764/UnreleasedLock.ql",
       "Successful pthread/mtx and exact standard mutex lock/try_lock "
       "operations; "
       "per-object CFG lock counts and failure-return alternatives, including "
       "direct helper calls. Only objects with a modeled unlock attempt are "
       "reported. Unknown aliases, arbitrary custom mutex APIs, RAII and "
       "unbounded lock-count growth remain excluded/incomplete."},
      {"cpp/jpl-c/release-locks-when-acquired", "warning",
       "JPL_C/LOC-2/Rule 09/ReleaseLocksWhenAcquired.ql",
       "Successful VxWorks semTake/taskLock/intLock/taskRtpLock; exact handle "
       "identity and matching semGive/taskUnlock/intUnlock/taskRtpUnlock in "
       "the acquiring function. Each lock site retains its own unmatched "
       "obligation. Scalable API variants and unknown handle aliases "
       "excluded."},
      {"cpp/twice-locked", "error", "Security/CWE/CWE-764/TwiceLocked.ql",
       "Known standard nonrecursive mutex blocking lock operations with a "
       "positive per-object CFG lock count, including direct helper calls. "
       "try_lock, recursive_mutex and pthread/mtx objects with unknown "
       "recursion "
       "attributes are excluded; no lock-order cycle inference."}};
  return rules;
}

LifetimeAnalysisResult LifetimeAnalysis::analyze(const Module &module,
                                           size_t max_states_per_object) const {
  LifetimeAnalysisResult result;
  SummaryCache summaries;
  std::map<std::pair<const Function *, Family>, bool> release_memo;
  std::map<const Function *, bool> mutex_memo;
  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    std::vector<Object> objects;
    bool has_memory_release = false;
    bool has_file_release = false;
    bool has_descriptor_release = false;
    for (const BasicBlock &block : function)
      for (const Instruction &instruction : block) {
        if (const auto *call = dyn_cast<CallBase>(&instruction)) {
          has_memory_release |= releases(*call, Family::Memory);
          has_file_release |= releases(*call, Family::File);
          has_descriptor_release |= releases(*call, Family::Descriptor);
          if (const Function *callee = ValueFacts::callee(*call))
            has_memory_release |= named(*callee, "realloc");
        }
        // Invoke acquisition needs separate normal/unwind return states.
        if (const auto *call = dyn_cast<CallInst>(&instruction)) {
          if (auto family = acquireFamily(*call))
            objects.push_back({call, call, *family});
          else if (auto model = lockModel(*call))
            if (model->acquire)
              objects.push_back(
                  {lockValue(*call, *model), call, model->family});
        }
      }
    if (!has_memory_release) {
      std::set<const Function *> visited;
      has_memory_release =
          reachesRelease(function, Family::Memory, visited, release_memo);
      if (!has_memory_release)
        for (const Function *v : visited)
          release_memo[{v, Family::Memory}] = false;
    }
    if (!has_file_release) {
      std::set<const Function *> visited;
      has_file_release =
          reachesRelease(function, Family::File, visited, release_memo);
      if (!has_file_release)
        for (const Function *v : visited)
          release_memo[{v, Family::File}] = false;
    }
    if (!has_descriptor_release) {
      std::set<const Function *> visited;
      has_descriptor_release =
          reachesRelease(function, Family::Descriptor, visited, release_memo);
      if (!has_descriptor_release)
        for (const Function *v : visited)
          release_memo[{v, Family::Descriptor}] = false;
    }
    // External parameters matter for flow-after-free rules, but never for
    // acquisition leaks. Avoid exploring every ordinary pointer argument.
    if (has_memory_release)
      for (const Argument &argument : function.args())
        if (argument.getType()->isPointerTy())
          objects.push_back({&argument, nullptr, Family::Memory});
    if (has_file_release || has_descriptor_release)
      for (const Argument &argument : function.args()) {
        if (has_file_release && argument.getType()->isPointerTy())
          objects.push_back({&argument, nullptr, Family::File});
        if (has_descriptor_release && argument.getType()->isIntegerTy())
          objects.push_back({&argument, nullptr, Family::Descriptor});
      }
    if (std::none_of(objects.begin(), objects.end(), [](const Object &object) {
          return object.family == Family::Mutex;
        })) {
      std::set<const Function *> visited;
      bool reaches_lock = reachesMutexLock(function, visited, mutex_memo);
      if (!reaches_lock)
        for (const Function *v : visited)
          mutex_memo[v] = false;
      if (reaches_lock)
        // A caller with only wrapper acquisitions still carries lock counts
        // through matched calls. It has no synthetic acquisition finding;
        // concrete blocking sites in the callee retain the source identity.
        objects.push_back({nullptr, nullptr, Family::Mutex});
    }
    for (const Object &object : objects) {
      Explorer explorer{function, module.getDataLayout(),
                        object,   max_states_per_object,
                        result,   summaries};
      explorer.run();
    }
  }
  // Different origins can converge to the same reported source location. Keep
  // one finding per rule and LLVM site, retaining a concrete release witness.
  std::set<std::pair<std::string, const Instruction *>> seen;
  std::vector<AnalysisFinding> unique;
  for (AnalysisFinding &finding : result.findings)
    if (seen.insert({finding.rule_id, finding.site}).second)
      unique.push_back(std::move(finding));
  result.findings = std::move(unique);
  return result;
}

} // namespace pdg
