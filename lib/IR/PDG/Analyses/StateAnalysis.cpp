#include "IR/PDG/Analyses/StateAnalysis.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"

#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Models/ValueFacts.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <set>
#include <tuple>

using namespace llvm;

namespace pdg {
namespace {

constexpr unsigned Null = 1, NonNull = 2, UnknownNull = 4;
constexpr unsigned Uninit = 1, Init = 2, UnknownInit = 4;

struct ScalarDomain {
  int64_t lower = std::numeric_limits<int64_t>::min();
  int64_t upper = std::numeric_limits<int64_t>::max();
  std::set<int64_t> excluded;
  bool finite = false;
  std::set<int64_t> values;
  bool operator==(const ScalarDomain &other) const {
    return lower == other.lower && upper == other.upper &&
           excluded == other.excluded && finite == other.finite &&
           values == other.values;
  }
};

struct BooleanGuard {
  std::map<const Value *, ScalarDomain> when_true;
  std::map<const Value *, ScalarDomain> when_false;
  bool true_possible = false;
  bool false_possible = false;
  bool operator==(const BooleanGuard &other) const {
    return when_true == other.when_true && when_false == other.when_false &&
           true_possible == other.true_possible &&
           false_possible == other.false_possible;
  }
};

struct MemoryRegion {
  const Value *object = nullptr;
  int64_t offset = 0;
  uint64_t bytes = 0;
  bool operator<(const MemoryRegion &other) const {
    return std::tie(object, offset, bytes) <
           std::tie(other.object, other.offset, other.bytes);
  }
  bool operator==(const MemoryRegion &other) const {
    return object == other.object && offset == other.offset &&
           bytes == other.bytes;
  }
};

enum class ConditionalOutputKind { Scanf, DefinedInitializer };

struct PendingOutput {
  const CallBase *call = nullptr;
  ConditionalOutputKind kind = ConditionalOutputKind::Scanf;
  unsigned prior_initialization = UnknownInit;
  unsigned minimum_count = 0;
  std::set<int64_t> written_statuses;
  std::set<int64_t> unwritten_statuses;
  bool unknown_status = false;
  bool incorrectly_checked_scanf = false;
  bool output_compared_after_call = false;
  bool return_count_maps_output = true;
  bool operator==(const PendingOutput &other) const {
    return call == other.call && kind == other.kind &&
           prior_initialization == other.prior_initialization &&
           minimum_count == other.minimum_count &&
           written_statuses == other.written_statuses &&
           unwritten_statuses == other.unwritten_statuses &&
           unknown_status == other.unknown_status &&
           incorrectly_checked_scanf == other.incorrectly_checked_scanf &&
           output_compared_after_call == other.output_compared_after_call &&
           return_count_maps_output == other.return_count_maps_output;
  }
};

struct NullFact {
  unsigned bits = UnknownNull;
  // Identity relates a checked SSA pointer to copies still holding that value.
  // A strong store changes identity, so a check never validates a later store.
  const Value *identity = nullptr;
  std::set<const Instruction *> origins;
  const Instruction *definition = nullptr;
  bool ambiguous_definition = false;
  bool operator==(const NullFact &other) const {
    return bits == other.bits && identity == other.identity &&
           origins == other.origins && definition == other.definition &&
           ambiguous_definition == other.ambiguous_definition;
  }
};

struct Cell {
  unsigned initialization = UnknownInit;
  NullFact pointer;
  const Instruction *last_write = nullptr;
  bool escaped = false;
  const ConstantInt *integer = nullptr;
  const Value *scalar_source = nullptr;
  const Value *stored_pointer = nullptr;
  std::map<const CallBase *, PendingOutput> pending_outputs;
  bool operator==(const Cell &other) const {
    return initialization == other.initialization && pointer == other.pointer &&
           last_write == other.last_write && escaped == other.escaped &&
           integer == other.integer && scalar_source == other.scalar_source &&
           stored_pointer == other.stored_pointer &&
           pending_outputs == other.pending_outputs;
  }
};

struct State {
  bool reachable = false;
  std::map<const AllocaInst *, Cell> cells;
  std::map<const Value *, NullFact> pointers;
  std::map<const Value *, const ConstantInt *> integers;
  std::map<const Value *, const Value *> scalar_sources;
  std::map<const Value *, const Value *> pointer_copies;
  std::map<const Value *, ScalarDomain> domains;
  std::map<const Value *, BooleanGuard> boolean_guards;
  std::map<MemoryRegion, Cell> memory_cells;
  std::map<const Value *, unsigned> root_initialization;
  std::set<const CallBase *> checked_statuses;
  std::set<const Value *> checked_pointers;
  std::set<std::pair<const CallBase *, MemoryRegion>> reported_outputs;
  std::set<const Value *> aliased_objects;
  bool unsupported_guard = false;
  bool operator==(const State &other) const {
    return reachable == other.reachable && cells == other.cells &&
           pointers == other.pointers && integers == other.integers &&
           scalar_sources == other.scalar_sources &&
           pointer_copies == other.pointer_copies && domains == other.domains &&
           boolean_guards == other.boolean_guards &&
           memory_cells == other.memory_cells &&
           root_initialization == other.root_initialization &&
           checked_statuses == other.checked_statuses &&
           checked_pointers == other.checked_pointers &&
           reported_outputs == other.reported_outputs &&
           aliased_objects == other.aliased_objects &&
           unsupported_guard == other.unsupported_guard;
  }
};

using NullableFunctions = std::set<const Function *>;
using ParameterReads =
    std::map<const Function *, std::map<unsigned, const Instruction *>>;

NullFact mergeFact(const NullFact &a, const NullFact &b) {
  NullFact result = a;
  result.bits |= b.bits;
  if (a.identity != b.identity)
    result.identity = nullptr;
  if (a.definition != b.definition) {
    result.definition = nullptr;
    result.ambiguous_definition = true;
  }
  result.ambiguous_definition |= b.ambiguous_definition;
  result.origins.insert(b.origins.begin(), b.origins.end());
  return result;
}

ScalarDomain mergeDomain(const ScalarDomain &a, const ScalarDomain &b) {
  ScalarDomain result;
  result.lower = std::min(a.lower, b.lower);
  result.upper = std::max(a.upper, b.upper);
  std::set_intersection(a.excluded.begin(), a.excluded.end(),
                        b.excluded.begin(), b.excluded.end(),
                        std::inserter(result.excluded, result.excluded.end()));
  result.finite = a.finite && b.finite;
  if (result.finite) {
    result.values = a.values;
    result.values.insert(b.values.begin(), b.values.end());
  }
  return result;
}

void mergeDomains(std::map<const Value *, ScalarDomain> &to,
                  const std::map<const Value *, ScalarDomain> &from) {
  for (auto &entry : to) {
    auto found = from.find(entry.first);
    entry.second = mergeDomain(
        entry.second, found == from.end() ? ScalarDomain{} : found->second);
  }
  for (const auto &entry : from)
    if (!to.count(entry.first))
      to[entry.first] = mergeDomain(ScalarDomain{}, entry.second);
}

void mergeCell(Cell &cell, const Cell &other) {
  cell.initialization |= other.initialization;
  cell.pointer = mergeFact(cell.pointer, other.pointer);
  cell.escaped |= other.escaped;
  if (cell.integer != other.integer)
    cell.integer = nullptr;
  if (cell.scalar_source != other.scalar_source)
    cell.scalar_source = nullptr;
  if (cell.stored_pointer != other.stored_pointer)
    cell.stored_pointer = nullptr;
  if (cell.last_write != other.last_write)
    cell.last_write = nullptr;
  for (const auto &pending : other.pending_outputs)
    cell.pending_outputs.emplace(pending);
}

unsigned memoryDefault(const State &state, const MemoryRegion &region) {
  if (const auto *alloca = dyn_cast_or_null<AllocaInst>(region.object)) {
    auto found = state.cells.find(alloca);
    if (region.offset == 0 && found != state.cells.end() &&
        region.bytes == alloca->getModule()->getDataLayout().getTypeAllocSize(
                            alloca->getAllocatedType()))
      return found->second.initialization;
  }
  if (isa_and_nonnull<GlobalVariable>(region.object))
    return Init;
  auto found = state.root_initialization.find(region.object);
  return found == state.root_initialization.end() ? UnknownInit : found->second;
}

void mergeState(State &to, const State &from) {
  if (!from.reachable)
    return;
  if (!to.reachable) {
    to = from;
    return;
  }
  for (auto &entry : to.cells) {
    auto found = from.cells.find(entry.first);
    Cell other = found == from.cells.end() ? Cell{} : found->second;
    mergeCell(entry.second, other);
  }
  for (const auto &entry : from.cells)
    if (!to.cells.count(entry.first)) {
      Cell cell = entry.second;
      cell.initialization |= UnknownInit;
      cell.pointer.bits |= UnknownNull;
      cell.last_write = nullptr;
      to.cells.emplace(entry.first, cell);
    }
  for (auto &entry : to.pointers) {
    auto found = from.pointers.find(entry.first);
    entry.second =
        mergeFact(entry.second,
                  found == from.pointers.end() ? NullFact{} : found->second);
  }
  for (const auto &entry : from.pointers)
    if (!to.pointers.count(entry.first))
      to.pointers.emplace(entry.first, mergeFact(NullFact{}, entry.second));
  for (auto &entry : to.integers) {
    auto found = from.integers.find(entry.first);
    if (found == from.integers.end() || found->second != entry.second)
      entry.second = nullptr;
  }
  for (const auto &entry : from.integers)
    if (!to.integers.count(entry.first))
      to.integers[entry.first] = nullptr;
  auto mergeCopies = [](auto &destination, const auto &source) {
    for (auto &entry : destination) {
      auto found = source.find(entry.first);
      if (found == source.end() || found->second != entry.second)
        entry.second = nullptr;
    }
    for (const auto &entry : source)
      if (!destination.count(entry.first))
        destination[entry.first] = nullptr;
  };
  mergeCopies(to.scalar_sources, from.scalar_sources);
  mergeCopies(to.pointer_copies, from.pointer_copies);
  for (auto &entry : to.domains) {
    auto found = from.domains.find(entry.first);
    entry.second =
        mergeDomain(entry.second, found == from.domains.end() ? ScalarDomain{}
                                                              : found->second);
  }
  for (const auto &entry : from.domains)
    if (!to.domains.count(entry.first))
      to.domains[entry.first] = mergeDomain(ScalarDomain{}, entry.second);
  for (auto &entry : to.boolean_guards) {
    auto found = from.boolean_guards.find(entry.first);
    if (found == from.boolean_guards.end()) {
      entry.second = {};
      entry.second.true_possible = entry.second.false_possible = true;
      continue;
    }
    const BooleanGuard &other = found->second;
    if (entry.second.true_possible && other.true_possible)
      mergeDomains(entry.second.when_true, other.when_true);
    else if (other.true_possible)
      entry.second.when_true = other.when_true;
    if (entry.second.false_possible && other.false_possible)
      mergeDomains(entry.second.when_false, other.when_false);
    else if (other.false_possible)
      entry.second.when_false = other.when_false;
    entry.second.true_possible |= other.true_possible;
    entry.second.false_possible |= other.false_possible;
  }
  for (const auto &entry : from.boolean_guards)
    if (!to.boolean_guards.count(entry.first)) {
      BooleanGuard unknown;
      unknown.true_possible = unknown.false_possible = true;
      to.boolean_guards[entry.first] = std::move(unknown);
    }
  for (auto &entry : to.memory_cells) {
    auto found = from.memory_cells.find(entry.first);
    Cell other;
    if (found != from.memory_cells.end())
      other = found->second;
    else
      other.initialization = memoryDefault(from, entry.first);
    mergeCell(entry.second, other);
  }
  for (const auto &entry : from.memory_cells)
    if (!to.memory_cells.count(entry.first)) {
      Cell cell = entry.second;
      cell.initialization |= memoryDefault(to, entry.first);
      to.memory_cells.emplace(entry.first, std::move(cell));
    }
  for (auto &entry : to.root_initialization) {
    auto found = from.root_initialization.find(entry.first);
    entry.second |=
        found == from.root_initialization.end() ? UnknownInit : found->second;
  }
  for (const auto &entry : from.root_initialization)
    if (!to.root_initialization.count(entry.first))
      to.root_initialization[entry.first] = entry.second | UnknownInit;
  std::set<const CallBase *> checked;
  std::set_intersection(to.checked_statuses.begin(), to.checked_statuses.end(),
                        from.checked_statuses.begin(),
                        from.checked_statuses.end(),
                        std::inserter(checked, checked.end()));
  to.checked_statuses = std::move(checked);
  std::set<const Value *> checked_pointers;
  std::set_intersection(
      to.checked_pointers.begin(), to.checked_pointers.end(),
      from.checked_pointers.begin(), from.checked_pointers.end(),
      std::inserter(checked_pointers, checked_pointers.end()));
  to.checked_pointers = std::move(checked_pointers);
  std::set<std::pair<const CallBase *, MemoryRegion>> reported;
  std::set_intersection(to.reported_outputs.begin(), to.reported_outputs.end(),
                        from.reported_outputs.begin(),
                        from.reported_outputs.end(),
                        std::inserter(reported, reported.end()));
  to.reported_outputs = std::move(reported);
  to.aliased_objects.insert(from.aliased_objects.begin(),
                            from.aliased_objects.end());
  to.unsupported_guard |= from.unsupported_guard;
}

// Only complete scalar cells are tracked: a byte store into an int is not a
// strong initialization of the int, and aggregate field initialization is a
// separate memory-range problem.
const AllocaInst *scalarCell(const Value *pointer, const DataLayout &layout,
                             const Type *access_type = nullptr) {
  if (!pointer || !pointer->getType()->isPointerTy())
    return nullptr;
  int64_t offset = 0;
  const Value *base = GetPointerBaseWithConstantOffset(pointer, offset, layout);
  const auto *alloca = dyn_cast<AllocaInst>(base);
  if (!alloca || offset != 0 || alloca->isArrayAllocation() ||
      !alloca->getAllocatedType()->isSingleValueType() ||
      !alloca->getAllocatedType()->isSized())
    return nullptr;
  if (access_type &&
      (!access_type->isSized() ||
       layout.getTypeStoreSize(const_cast<Type *>(access_type)) !=
           layout.getTypeAllocSize(alloca->getAllocatedType())))
    return nullptr;
  return alloca;
}

NullFact pointerFact(const Value *value, const State &state,
                     const NullableFunctions &nullable, unsigned depth = 0) {
  if (!value || !value->getType()->isPointerTy() || depth > 20)
    return {};
  auto known = state.pointers.find(value);
  if (known != state.pointers.end())
    return known->second;
  if (isa<ConstantPointerNull>(value))
    return {Null, value, {}};
  if (isa<AllocaInst>(value) || isa<GlobalValue>(value))
    return {NonNull, value, {}};
  if (const auto *call = dyn_cast<CallBase>(value)) {
    const Function *callee = ValueFacts::callee(*call);
    if (callee && nullable.count(callee))
      return {Null | NonNull, value, {call}};
    if (call->hasRetAttr(Attribute::NonNull))
      return {NonNull, value, {}};
    return {UnknownNull, value, {}};
  }
  if (const auto *gep = dyn_cast<GEPOperator>(value))
    return pointerFact(gep->getPointerOperand(), state, nullable, depth + 1);
  if (const auto *cast = dyn_cast<Operator>(value))
    if (cast->getOpcode() == Instruction::BitCast ||
        cast->getOpcode() == Instruction::AddrSpaceCast)
      return pointerFact(cast->getOperand(0), state, nullable, depth + 1);
  if (const auto *select = dyn_cast<SelectInst>(value)) {
    NullFact a =
        pointerFact(select->getTrueValue(), state, nullable, depth + 1);
    NullFact b =
        pointerFact(select->getFalseValue(), state, nullable, depth + 1);
    NullFact fact = mergeFact(a, b);
    fact.identity = value;
    return fact;
  }
  return {UnknownNull, value, {}};
}

const ConstantInt *integerFact(const Value *value, const State &state,
                               unsigned depth = 0) {
  if (!value || !value->getType()->isIntegerTy() || depth > 12)
    return nullptr;
  if (const auto *constant = dyn_cast<ConstantInt>(value))
    return constant;
  auto found = state.integers.find(value);
  if (found != state.integers.end())
    return found->second;
  if (const auto *cast = dyn_cast<CastInst>(value))
    if (cast->getOperand(0)->getType()->isIntegerTy())
      if (const ConstantInt *operand =
              integerFact(cast->getOperand(0), state, depth + 1))
        return dyn_cast<ConstantInt>(ConstantExpr::getCast(
            cast->getOpcode(), const_cast<ConstantInt *>(operand),
            cast->getType()));
  if (const auto *comparison = dyn_cast<ICmpInst>(value)) {
    const ConstantInt *a =
        integerFact(comparison->getOperand(0), state, depth + 1);
    const ConstantInt *b =
        integerFact(comparison->getOperand(1), state, depth + 1);
    if (a && b)
      return dyn_cast<ConstantInt>(ConstantExpr::getICmp(
          comparison->getPredicate(), const_cast<ConstantInt *>(a),
          const_cast<ConstantInt *>(b)));
  }
  if (const auto *binary = dyn_cast<BinaryOperator>(value)) {
    unsigned opcode = binary->getOpcode();
    if (opcode == Instruction::And || opcode == Instruction::Or ||
        opcode == Instruction::Xor || opcode == Instruction::Add ||
        opcode == Instruction::Sub || opcode == Instruction::Mul) {
      const ConstantInt *a =
          integerFact(binary->getOperand(0), state, depth + 1);
      const ConstantInt *b =
          integerFact(binary->getOperand(1), state, depth + 1);
      if (a && b)
        return dyn_cast<ConstantInt>(
            ConstantExpr::get(opcode, const_cast<ConstantInt *>(a),
                              const_cast<ConstantInt *>(b)));
    }
  }
  return nullptr;
}

const Value *scalarSource(const Value *value, const State &state,
                          unsigned depth = 0) {
  if (!value || !value->getType()->isIntegerTy() || depth > 24)
    return nullptr;
  auto found = state.scalar_sources.find(value);
  if (found != state.scalar_sources.end()) {
    if (!found->second)
      return nullptr;
    if (found->second != value)
      return scalarSource(found->second, state, depth + 1);
  }
  if (const auto *cast = dyn_cast<CastInst>(value)) {
    if (cast->getOperand(0)->getType()->isIntegerTy()) {
      const Value *origin = scalarSource(cast->getOperand(0), state, depth + 1);
      if (cast->getOpcode() == Instruction::SExt &&
          !cast->getOperand(0)->getType()->isIntegerTy(1))
        return origin;
      if (cast->getOpcode() == Instruction::ZExt && origin) {
        auto domain = state.domains.find(origin);
        if (origin->getType()->isIntegerTy(1) ||
            (domain != state.domains.end() && domain->second.lower >= 0))
          return origin;
      }
      if (cast->getOpcode() == Instruction::Trunc &&
          cast->getType()->isIntegerTy(1) && origin &&
          origin->getType()->isIntegerTy(1))
        return origin;
    }
  }
  return value;
}

int64_t signedConstant(const ConstantInt &constant) {
  return constant.getBitWidth() == 1 ? constant.getZExtValue()
                                     : constant.getSExtValue();
}

Optional<uint64_t> objectExtent(const Value &object) {
  if (auto extent = ValueFacts::objectBytes(object))
    return extent;
  const auto *call = dyn_cast<CallBase>(&object);
  const Function *callee = call ? ValueFacts::callee(*call) : nullptr;
  if (!callee)
    return None;
  auto argument = [&](unsigned index) -> Optional<uint64_t> {
    if (index >= call->arg_size())
      return None;
    const auto *constant = dyn_cast<ConstantInt>(call->getArgOperand(index));
    if (!constant || constant->getValue().getActiveBits() > 64)
      return None;
    return constant->getZExtValue();
  };
  if (ValueFacts::hasLibraryName(*callee, "calloc", true)) {
    auto count = argument(0), size = argument(1);
    if (!count || !size ||
        (*size && *count > std::numeric_limits<uint64_t>::max() / *size))
      return None;
    return *count * *size;
  }
  if (ValueFacts::hasLibraryName(*callee, "malloc", true) ||
      LibraryModels::allocation(*callee) == AllocationKind::New ||
      LibraryModels::allocation(*callee) == AllocationKind::NewArray)
    return argument(0);
  if (ValueFacts::hasLibraryName(*callee, "aligned_alloc", true) ||
      ValueFacts::hasLibraryName(*callee, "realloc", true))
    return argument(1);
  return None;
}

Optional<MemoryRegion> memoryRegion(const Value *pointer, uint64_t bytes,
                                    const State &state,
                                    const DataLayout &layout,
                                    unsigned depth = 0) {
  if (!pointer || !pointer->getType()->isPointerTy() || !bytes || depth > 24)
    return None;
  int64_t offset = 0;
  const Value *base = GetPointerBaseWithConstantOffset(pointer, offset, layout);
  auto copy = state.pointer_copies.find(base);
  if (copy != state.pointer_copies.end()) {
    if (!copy->second || copy->second == base)
      return None;
    auto result = memoryRegion(copy->second, bytes, state, layout, depth + 1);
    if (!result || offset < 0 ||
        result->offset > std::numeric_limits<int64_t>::max() - offset)
      return None;
    result->offset += offset;
    if (auto extent = objectExtent(*result->object))
      if (static_cast<uint64_t>(result->offset) > *extent ||
          result->bytes > *extent - static_cast<uint64_t>(result->offset))
        return None;
    return result;
  }
  if (offset < 0)
    return None;
  bool known = isa<AllocaInst>(base) || isa<GlobalVariable>(base);
  if (const auto *call = dyn_cast<CallBase>(base))
    if (const Function *callee = ValueFacts::callee(*call))
      known = LibraryModels::allocation(*callee) != AllocationKind::Unknown;
  if (!known)
    return None;
  if (auto extent = objectExtent(*base))
    if (static_cast<uint64_t>(offset) > *extent ||
        bytes > *extent - static_cast<uint64_t>(offset))
      return None;
  return MemoryRegion{base, offset, bytes};
}

Optional<MemoryRegion> memoryRegion(const Value *pointer, Type *type,
                                    const State &state,
                                    const DataLayout &layout) {
  if (!type || !type->isSized())
    return None;
  auto size = layout.getTypeStoreSize(type);
  return size.isScalable()
             ? None
             : memoryRegion(pointer, size.getFixedValue(), state, layout);
}

ScalarDomain domainFor(const Value *source, const State &state) {
  auto found = state.domains.find(source);
  if (found != state.domains.end())
    return found->second;
  ScalarDomain domain;
  if (source && source->getType()->isIntegerTy()) {
    unsigned bits = source->getType()->getIntegerBitWidth();
    if (bits == 1) {
      domain.lower = 0;
      domain.upper = 1;
    } else if (bits < 64) {
      domain.lower = -(int64_t(1) << (bits - 1));
      domain.upper = (int64_t(1) << (bits - 1)) - 1;
    }
  }
  return domain;
}

bool contains(const ScalarDomain &domain, int64_t value) {
  return value >= domain.lower && value <= domain.upper &&
         !domain.excluded.count(value) &&
         (!domain.finite || domain.values.count(value));
}

ScalarDomain intersectDomain(const ScalarDomain &a, const ScalarDomain &b) {
  ScalarDomain result;
  result.lower = std::max(a.lower, b.lower);
  result.upper = std::min(a.upper, b.upper);
  result.excluded = a.excluded;
  result.excluded.insert(b.excluded.begin(), b.excluded.end());
  result.finite = a.finite || b.finite;
  if (result.finite) {
    const auto &candidates = a.finite ? a.values : b.values;
    for (int64_t value : candidates)
      if (contains(a, value) && contains(b, value))
        result.values.insert(value);
  }
  return result;
}

bool restrictDomain(ScalarDomain &domain, ICmpInst::Predicate predicate,
                    int64_t value) {
  auto minimum = std::numeric_limits<int64_t>::min();
  auto maximum = std::numeric_limits<int64_t>::max();
  switch (predicate) {
  case ICmpInst::ICMP_EQ:
    domain.lower = std::max(domain.lower, value);
    domain.upper = std::min(domain.upper, value);
    break;
  case ICmpInst::ICMP_NE:
    domain.excluded.insert(value);
    break;
  case ICmpInst::ICMP_SGE:
    domain.lower = std::max(domain.lower, value);
    break;
  case ICmpInst::ICMP_SGT:
    if (value == maximum)
      return false;
    domain.lower = std::max(domain.lower, value + 1);
    break;
  case ICmpInst::ICMP_SLE:
    domain.upper = std::min(domain.upper, value);
    break;
  case ICmpInst::ICMP_SLT:
    if (value == minimum)
      return false;
    domain.upper = std::min(domain.upper, value - 1);
    break;
  default:
    return true;
  }
  if (domain.lower > domain.upper)
    return false;
  if (domain.finite) {
    for (auto it = domain.values.begin(); it != domain.values.end();) {
      if (!contains(domain, *it))
        it = domain.values.erase(it);
      else
        ++it;
    }
    return !domain.values.empty();
  }
  if (domain.lower == domain.upper && domain.excluded.count(domain.lower))
    return false;
  return true;
}

bool outputGuaranteed(const PendingOutput &pending,
                      const ScalarDomain &domain) {
  if (pending.kind == ConditionalOutputKind::Scanf) {
    if (!pending.return_count_maps_output)
      return false;
    if (domain.lower >= static_cast<int64_t>(pending.minimum_count))
      return true;
    if (domain.finite && !domain.values.empty())
      return std::all_of(
          domain.values.begin(), domain.values.end(), [&](int64_t value) {
            return value >= static_cast<int64_t>(pending.minimum_count);
          });
    // A succession of range and inequality checks can prove rc >= n.
    int64_t low = domain.lower;
    while (low < static_cast<int64_t>(pending.minimum_count) &&
           domain.excluded.count(low))
      ++low;
    return low >= static_cast<int64_t>(pending.minimum_count);
  }
  if (pending.unknown_status || pending.written_statuses.empty())
    return false;
  bool possible = false;
  for (int64_t value : pending.written_statuses)
    possible |= contains(domain, value);
  for (int64_t value : pending.unwritten_statuses)
    if (contains(domain, value))
      return false;
  return possible;
}

void updateConditionalCells(State &state) {
  auto update = [&](Cell &cell) {
    for (const auto &entry : cell.pending_outputs) {
      const PendingOutput &pending = entry.second;
      if (outputGuaranteed(pending, domainFor(pending.call, state))) {
        cell.initialization = Init;
        cell.pointer = {};
        cell.integer = nullptr;
        cell.scalar_source = nullptr;
        cell.stored_pointer = nullptr;
      }
    }
  };
  for (auto &entry : state.cells)
    update(entry.second);
  for (auto &entry : state.memory_cells)
    update(entry.second);
}

bool refineScalar(State &state, const Value *operand,
                  ICmpInst::Predicate predicate, int64_t value) {
  const Value *source = scalarSource(operand, state);
  if (!source || !source->getType()->isIntegerTy() ||
      source->getType()->getIntegerBitWidth() > 64)
    return false;
  if (!(isa<Argument>(source) || isa<CallBase>(source) ||
        isa<LoadInst>(source)))
    return false;
  ScalarDomain domain = domainFor(source, state);
  if (ICmpInst::isUnsigned(predicate)) {
    bool nonnegative = domain.lower >= 0 ||
                       (domain.finite && !domain.values.empty() &&
                        std::all_of(domain.values.begin(), domain.values.end(),
                                    [](int64_t item) { return item >= 0; }));
    if (!nonnegative || value < 0)
      return false;
    predicate = ICmpInst::getSignedPredicate(predicate);
  }
  if (!restrictDomain(domain, predicate, value))
    state.reachable = false;
  state.domains[source] = std::move(domain);
  if (const auto *call = dyn_cast<CallBase>(source))
    state.checked_statuses.insert(call);
  updateConditionalCells(state);
  return true;
}

bool isNullable(const NullFact &fact) {
  return (fact.bits & Null) && !(fact.bits & UnknownNull);
}

bool hasCallOrigin(const NullFact &fact) {
  return std::any_of(
      fact.origins.begin(), fact.origins.end(),
      [](const Instruction *origin) { return isa<CallBase>(origin); });
}

const Argument *parameterOrigin(const Value *pointer, const Function &function,
                                const DominatorTree &dominators,
                                unsigned depth = 0) {
  if (depth > 24)
    return nullptr;
  const Value *root = getUnderlyingObject(pointer);
  if (const auto *argument = dyn_cast<Argument>(root))
    return argument->getParent() == &function ? argument : nullptr;
  // Recognize an unmodified frontend parameter spill, not a general pointer
  // load or a callee return. Every use must preserve the private spill slot.
  const auto *load = dyn_cast<LoadInst>(root);
  if (!load)
    return nullptr;
  const auto *slot =
      dyn_cast<AllocaInst>(load->getPointerOperand()->stripPointerCasts());
  if (!slot || !slot->getAllocatedType()->isPointerTy())
    return nullptr;
  const StoreInst *definition = nullptr;
  for (const User *user : slot->users()) {
    if (const auto *store = dyn_cast<StoreInst>(user)) {
      if (store->getPointerOperand() != slot || definition)
        return nullptr;
      definition = store;
    } else if (const auto *read = dyn_cast<LoadInst>(user)) {
      if (read->getPointerOperand() != slot)
        return nullptr;
    } else if (!isa<DbgInfoIntrinsic>(user)) {
      return nullptr;
    }
  }
  if (!definition || !dominators.dominates(definition, load))
    return nullptr;
  int64_t offset = 0;
  const Value *source =
      GetPointerBaseWithConstantOffset(definition->getValueOperand(), offset,
                                       function.getParent()->getDataLayout());
  // Zero-offset private pointer copies are transparent. A nonzero or dynamic
  // GEP hidden in a spill needs explicit offset propagation and stays unknown.
  if (offset != 0 || isa<GEPOperator>(source))
    return nullptr;
  return parameterOrigin(source, function, dominators, depth + 1);
}

// Exact standard memory API roles; nullable buffers are only treated as read
// when a retained nonzero transfer requires the buffer to be dereferenced.
std::vector<unsigned> memoryArguments(const CallBase &call) {
  if (const auto *transfer = dyn_cast<MemTransferInst>(&call)) {
    const auto *length = dyn_cast<ConstantInt>(transfer->getLength());
    return length && !length->isZero() ? std::vector<unsigned>{0, 1}
                                       : std::vector<unsigned>{};
  }
  if (const auto *set = dyn_cast<MemSetInst>(&call)) {
    const auto *length = dyn_cast<ConstantInt>(set->getLength());
    return length && !length->isZero() ? std::vector<unsigned>{0}
                                       : std::vector<unsigned>{};
  }
  const Function *callee = ValueFacts::callee(call);
  if (!callee || call.arg_size() < 3)
    return {};
  if (!ValueFacts::hasLibraryName(*callee, "memcpy", true) &&
      !ValueFacts::hasLibraryName(*callee, "memmove", true) &&
      !ValueFacts::hasLibraryName(*callee, "bcopy"))
    return {};
  const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2));
  return length && !length->isZero() ? std::vector<unsigned>{0, 1}
                                     : std::vector<unsigned>{};
}

struct ScanfOutput {
  unsigned argument = 0;
  unsigned minimum_count = 0;
  bool always_written = false;
  uint64_t bytes = 0;
  bool count_maps_output = true;
};

Optional<std::string> trackedString(const Value &value, const State &state,
                                    unsigned depth = 0) {
  if (depth > 24)
    return None;
  if (auto literal = ValueFacts::constantString(value))
    return literal;
  auto copy = state.pointer_copies.find(&value);
  if (copy != state.pointer_copies.end() && copy->second &&
      copy->second != &value)
    return trackedString(*copy->second, state, depth + 1);
  const Value *stripped = value.stripPointerCasts();
  if (stripped != &value)
    return trackedString(*stripped, state, depth + 1);
  return None;
}

bool scanfWhitespace(char c) { return StringRef(" \t\n\r\v\f").contains(c); }

// A deliberately small, target-independent constant-input proof. Only plain
// integral conversions fitting the scanf return type's C-int width are used;
// unsupported modifiers/conversions remain unknown. This proves reachability
// of a particular %n directive, not arbitrary sscanf result values.
bool literalPrefixReaches(const CallBase &call, StringRef format,
                          size_t target) {
  const Function *callee = ValueFacts::callee(call);
  if (!callee || !call.arg_size() || !call.getType()->isIntegerTy() ||
      call.getType()->getIntegerBitWidth() > 64 ||
      !(ValueFacts::hasLibraryName(*callee, "sscanf", true) ||
        ValueFacts::hasLibraryName(*callee, "swscanf", true) ||
        ValueFacts::hasLibraryName(*callee, "__isoc99_sscanf")))
    return false;
  auto input = ValueFacts::constantString(*call.getArgOperand(0));
  if (!input)
    return false;
  size_t cursor = 0;
  auto digit = [](char c) -> unsigned {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return 100;
  };
  for (size_t i = 0; i < format.size(); ++i) {
    if (i == target)
      return true;
    if (scanfWhitespace(format[i])) {
      while (cursor < input->size() && scanfWhitespace((*input)[cursor]))
        ++cursor;
      continue;
    }
    if (format[i] != '%') {
      if (cursor >= input->size() || (*input)[cursor++] != format[i])
        return false;
      continue;
    }
    if (++i == format.size())
      return false;
    if (format[i] == '%') {
      if (cursor >= input->size() || (*input)[cursor++] != '%')
        return false;
      continue;
    }
    if (format[i] == '*')
      ++i;
    size_t width = std::numeric_limits<size_t>::max();
    if (i < format.size() &&
        std::isdigit(static_cast<unsigned char>(format[i]))) {
      width = 0;
      while (i < format.size() &&
             std::isdigit(static_cast<unsigned char>(format[i]))) {
        if (width > 100000)
          return false;
        width = width * 10 + format[i++] - '0';
      }
    }
    if (i >= format.size())
      return false;
    char conversion = format[i];
    if (conversion == 'n')
      continue;
    if (!StringRef("diouxX").contains(conversion))
      return false;
    while (cursor < input->size() && scanfWhitespace((*input)[cursor]))
      ++cursor;
    size_t begin = cursor;
    bool negative = false;
    if (cursor < input->size() &&
        ((*input)[cursor] == '+' || (*input)[cursor] == '-')) {
      negative = (*input)[cursor++] == '-';
    }
    unsigned base = conversion == 'o'                          ? 8
                    : (conversion == 'x' || conversion == 'X') ? 16
                                                               : 10;
    if (conversion == 'i' && cursor < input->size() && (*input)[cursor] == '0')
      base = 8;
    if ((base == 16 || conversion == 'i') && cursor + 1 < input->size() &&
        (*input)[cursor] == '0' &&
        ((*input)[cursor + 1] == 'x' || (*input)[cursor + 1] == 'X')) {
      base = 16;
      cursor += 2;
    }
    unsigned bits = call.getType()->getIntegerBitWidth();
    bool signed_conversion = conversion == 'd' || conversion == 'i';
    uint64_t limit = std::numeric_limits<uint64_t>::max();
    if (signed_conversion)
      limit = negative ? uint64_t(1) << (bits - 1)
                       : (uint64_t(1) << (bits - 1)) - 1;
    else if (bits < 64)
      limit = (uint64_t(1) << bits) - 1;
    uint64_t value = 0;
    size_t digits = 0;
    while (cursor < input->size() && cursor - begin < width &&
           digit((*input)[cursor]) < base) {
      unsigned part = digit((*input)[cursor]);
      if (part > limit || value > (limit - part) / base)
        return false;
      value = value * base + part;
      ++cursor;
      ++digits;
    }
    if (!digits)
      return false;
  }
  return false;
}

std::vector<ScanfOutput> scanfOutputs(const CallBase &call,
                                      const State &state) {
  const Function *callee = ValueFacts::callee(call);
  if (!callee)
    return {};
  auto model = LibraryModels::format(*callee);
  if (!model || !model->scanf || model->format >= call.arg_size())
    return {};
  auto format = trackedString(*call.getArgOperand(model->format), state);
  std::vector<ScanfOutput> result;
  if (!format) {
    for (unsigned i = model->first_argument; i < call.arg_size(); ++i)
      if (call.getArgOperand(i)->getType()->isPointerTy())
        result.push_back({i, i - model->first_argument + 1});
    if (result.size() > 1)
      for (auto &output : result)
        output.count_maps_output = false;
    return result;
  }
  unsigned argument = 0, count = 0;
  bool input_barrier = false;
  for (size_t i = 0; i < format->size(); ++i) {
    if ((*format)[i] != '%') {
      if (!scanfWhitespace((*format)[i]))
        input_barrier = true;
      continue;
    }
    size_t conversion_start = i;
    if (++i == format->size())
      return {};
    if ((*format)[i] == '%') {
      input_barrier = true;
      continue;
    }
    bool suppressed = false;
    if ((*format)[i] == '*') {
      suppressed = true;
      ++i;
    }
    unsigned number = 0;
    while (i < format->size() &&
           std::isdigit(static_cast<unsigned char>((*format)[i]))) {
      unsigned digit = (*format)[i++] - '0';
      if (number > 100000)
        return {};
      number = number * 10 + digit;
    }
    unsigned position = argument;
    if (i < format->size() && (*format)[i] == '$') {
      if (!number)
        return {};
      position = number - 1;
      ++i;
      if (i < format->size() && (*format)[i] == '*') {
        suppressed = true;
        ++i;
      }
      while (i < format->size() &&
             std::isdigit(static_cast<unsigned char>((*format)[i])))
        ++i;
    }
    bool modified = false;
    while (i < format->size() && StringRef("hljztL").contains((*format)[i])) {
      modified = true;
      ++i;
    }
    if (i >= format->size())
      return {};
    char conversion = (*format)[i];
    if (conversion == '[') {
      ++i;
      if (i < format->size() && (*format)[i] == '^')
        ++i;
      if (i < format->size() && (*format)[i] == ']')
        ++i;
      while (i < format->size() && (*format)[i] != ']')
        ++i;
      if (i >= format->size())
        return {};
    } else if (!StringRef("diouxXaAeEfFgGcspnCS").contains(conversion))
      return {};
    if (suppressed) {
      input_barrier = true;
      continue;
    }
    ++argument;
    bool always_written =
        conversion == 'n' &&
        ((count == 0 && !input_barrier) ||
         literalPrefixReaches(call, *format, conversion_start));
    unsigned minimum_count =
        count + ((conversion == 'n' && input_barrier) ? 1 : 0);
    if (conversion != 'n') {
      ++count;
      minimum_count = count;
      input_barrier = false;
    }
    // Buffer conversion widths are byte-range effects rather than scalar
    // writes. They still consume a return-value assignment count.
    if (conversion == 's' || conversion == 'c' || conversion == '[' ||
        conversion == 'S' || conversion == 'C')
      continue;
    unsigned index = model->first_argument + position;
    if (index < call.arg_size() &&
        call.getArgOperand(index)->getType()->isPointerTy()) {
      uint64_t bytes = 0;
      if (!modified && StringRef("diouxXn").contains(conversion) &&
          call.getType()->isIntegerTy())
        bytes = call.getType()->getIntegerBitWidth() / 8;
      if (conversion == 'p')
        bytes = call.getModule()->getDataLayout().getPointerSize(0);
      result.push_back({index, minimum_count, always_written, bytes});
    }
  }
  return result;
}

const Value *staticScalarSource(const Value *value, unsigned depth = 0) {
  if (!value || depth > 24)
    return nullptr;
  if (const auto *cast = dyn_cast<CastInst>(value))
    if (cast->getOperand(0)->getType()->isIntegerTy())
      return staticScalarSource(cast->getOperand(0), depth + 1);
  if (const auto *load = dyn_cast<LoadInst>(value)) {
    const auto *slot =
        dyn_cast<AllocaInst>(load->getPointerOperand()->stripPointerCasts());
    if (!slot)
      return value;
    const StoreInst *definition = nullptr;
    for (const User *user : slot->users())
      if (const auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getPointerOperand() != slot || definition)
          return value;
        definition = store;
      } else if (!isa<LoadInst>(user) && !isa<DbgInfoIntrinsic>(user))
        return value;
    if (definition)
      return staticScalarSource(definition->getValueOperand(), depth + 1);
  }
  return value;
}

bool incorrectlyCheckedScanf(const CallBase &call) {
  bool zero = false, stronger = false;
  for (const BasicBlock &block : *call.getFunction())
    for (const Instruction &instruction : block)
      if (const auto *comparison = dyn_cast<ICmpInst>(&instruction)) {
        const Value *operand = comparison->getOperand(0);
        const auto *constant = dyn_cast<ConstantInt>(comparison->getOperand(1));
        auto predicate = comparison->getPredicate();
        if (!constant) {
          constant = dyn_cast<ConstantInt>(comparison->getOperand(0));
          operand = comparison->getOperand(1);
          predicate = ICmpInst::getSwappedPredicate(predicate);
        }
        if (!constant || constant->getBitWidth() > 64 ||
            staticScalarSource(operand) != &call)
          continue;
        int64_t value = signedConstant(*constant);
        zero |= comparison->isEquality() && value == 0;
        stronger |= (comparison->isEquality() && value > 0) ||
                    (ICmpInst::isSigned(predicate) && value >= 0);
      }
  const Module &module = *call.getModule();
  if (ValueFacts::macroDefined(module, "_LINUX_KERNEL_SPRINTF_H_") ||
      ValueFacts::macroDefined(module, "_LINUX_KERNEL_H"))
    return false;
  return zero && !stronger;
}

struct OutputWrite {
  unsigned argument = 0;
  int64_t offset = 0;
  uint64_t bytes = 0;
  bool operator<(const OutputWrite &other) const {
    return std::tie(argument, offset, bytes) <
           std::tie(other.argument, other.offset, other.bytes);
  }
};

struct OutputExit {
  Optional<int64_t> status;
  std::set<OutputWrite> writes;
  std::map<unsigned, unsigned> null_requirements;
};

using OutputSummaries = std::map<const Function *, std::vector<OutputExit>>;

void refineCondition(State &state, const Value *condition, bool truth,
                     const NullableFunctions &nullable, unsigned depth = 0);

Optional<OutputWrite> parameterWrite(const Value *pointer, uint64_t bytes,
                                     const Function &function,
                                     const DominatorTree &dominators) {
  int64_t offset = 0;
  const Value *base = GetPointerBaseWithConstantOffset(
      pointer, offset, function.getParent()->getDataLayout());
  if (isa<GEPOperator>(base))
    return None; // Dynamic offsets are not offset zero.
  const Argument *argument = parameterOrigin(base, function, dominators);
  if (!argument || offset < 0 || !bytes)
    return None;
  return OutputWrite{argument->getArgNo(), offset, bytes};
}

bool compatibleExit(const OutputExit &exit, const CallBase &call,
                    const State &state, const NullableFunctions &nullable) {
  for (const auto &requirement : exit.null_requirements) {
    if (requirement.first >= call.arg_size())
      return false;
    unsigned fact =
        pointerFact(call.getArgOperand(requirement.first), state, nullable)
            .bits;
    if ((fact == Null || fact == NonNull) && fact != requirement.second)
      return false;
  }
  return true;
}

// Enumerate bounded acyclic paths while retaining each return code's output
// effects. Joining return slots before recording a status would lose the
// essential association between a successful return and its preceding stores.
std::vector<OutputExit> summarizeOutputs(const Function &function,
                                         const OutputSummaries &known,
                                         const NullableFunctions &nullable,
                                         bool &limit_hit) {
  if (function.isDeclaration() || !function.getReturnType()->isIntegerTy() ||
      function.getReturnType()->getIntegerBitWidth() > 64)
    return {};
  DominatorTree dominators(const_cast<Function &>(function));
  bool relevant = false;
  for (const BasicBlock &block : function)
    for (const Instruction &instruction : block) {
      if (const auto *store = dyn_cast<StoreInst>(&instruction))
        relevant |= parameterOrigin(store->getPointerOperand(), function,
                                    dominators) != nullptr;
      if (const auto *call = dyn_cast<CallBase>(&instruction)) {
        const Function *callee = ValueFacts::callee(*call);
        if (callee == &function)
          return {}; // Recursive summaries are withheld.
        if (known.count(callee))
          for (const Use &argument : call->args())
            if (argument->getType()->isPointerTy())
              relevant |= parameterOrigin(argument.get(), function,
                                          dominators) != nullptr;
      }
    }
  if (!relevant)
    return {};
  LoopInfo loops(dominators);
  if (!loops.empty())
    return {};
  const DataLayout &layout = function.getParent()->getDataLayout();
  struct Path {
    const BasicBlock *block = nullptr;
    const BasicBlock *predecessor = nullptr;
    State state;
    std::set<OutputWrite> writes;
    std::set<const BasicBlock *> visited;
  };
  Path initial;
  initial.block = &function.getEntryBlock();
  initial.state.reachable = true;
  for (const Argument &argument : function.args())
    if (argument.getType()->isPointerTy())
      initial.state.pointers[&argument] = {UnknownNull, &argument, {}};
  std::deque<Path> queue{initial};
  std::vector<OutputExit> exits;
  size_t steps = 0;
  bool wrote_anything = false;
  while (!queue.empty()) {
    if (++steps > 512) {
      limit_hit = true;
      return {};
    }
    Path path = std::move(queue.front());
    queue.pop_front();
    if (!path.state.reachable || !path.visited.insert(path.block).second)
      continue;
    std::vector<Path> active{std::move(path)};
    for (const Instruction &instruction : *active.front().block) {
      std::vector<Path> next;
      for (Path &current : active) {
        State &state = current.state;
        if (const auto *phi = dyn_cast<PHINode>(&instruction)) {
          int index = current.predecessor
                          ? phi->getBasicBlockIndex(current.predecessor)
                          : -1;
          if (index >= 0) {
            const Value *incoming = phi->getIncomingValue(index);
            if (phi->getType()->isIntegerTy()) {
              state.integers[phi] = integerFact(incoming, state);
              state.scalar_sources[phi] = scalarSource(incoming, state);
            } else if (phi->getType()->isPointerTy())
              state.pointers[phi] = pointerFact(incoming, state, nullable);
          }
        } else if (const auto *alloca = dyn_cast<AllocaInst>(&instruction)) {
          if (scalarCell(alloca, layout))
            state.cells[alloca] = {Uninit, {}, alloca};
          state.pointers[alloca] = {NonNull, alloca, {}};
        } else if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
          auto slot = state.cells.find(
              scalarCell(load->getPointerOperand(), layout, load->getType()));
          if (load->getType()->isIntegerTy()) {
            state.integers[load] =
                slot == state.cells.end() ? nullptr : slot->second.integer;
            state.scalar_sources[load] =
                slot == state.cells.end() ? load : slot->second.scalar_source;
          } else if (load->getType()->isPointerTy())
            state.pointers[load] =
                slot == state.cells.end() ? NullFact{} : slot->second.pointer;
        } else if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
          Type *type = store->getValueOperand()->getType();
          if (type->isPointerTy() &&
              !isa<AllocaInst>(
                  getUnderlyingObject(store->getPointerOperand())) &&
              parameterOrigin(store->getValueOperand(), function, dominators))
            return {}; // Captured output-pointer aliases need escape summaries.
          if (const AllocaInst *slot =
                  scalarCell(store->getPointerOperand(), layout, type)) {
            Cell &cell = state.cells[slot];
            cell.initialization = Init;
            cell.integer = integerFact(store->getValueOperand(), state);
            cell.scalar_source = scalarSource(store->getValueOperand(), state);
            cell.pointer =
                pointerFact(store->getValueOperand(), state, nullable);
          }
          if (type->isSized()) {
            auto bytes = layout.getTypeStoreSize(type);
            if (!bytes.isScalable())
              if (auto write = parameterWrite(store->getPointerOperand(),
                                              bytes.getFixedValue(), function,
                                              dominators)) {
                current.writes.insert(*write);
                wrote_anything = true;
              }
          }
        } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
          const Function *callee = ValueFacts::callee(*call);
          auto summary = known.find(callee);
          if (summary != known.end()) {
            for (const OutputExit &exit : summary->second) {
              if (!compatibleExit(exit, *call, state, nullable))
                continue;
              if (next.size() + queue.size() >= 512) {
                limit_hit = true;
                return {};
              }
              Path alternative = current;
              if (exit.status)
                alternative.state.integers[call] = ConstantInt::get(
                    cast<IntegerType>(call->getType()), *exit.status);
              for (const OutputWrite &effect : exit.writes)
                if (effect.argument < call->arg_size())
                  if (auto write =
                          parameterWrite(call->getArgOperand(effect.argument),
                                         effect.bytes, function, dominators)) {
                    if (effect.offset <=
                        std::numeric_limits<int64_t>::max() - write->offset) {
                      write->offset += effect.offset;
                      alternative.writes.insert(*write);
                      wrote_anything = true;
                    }
                  }
              next.push_back(std::move(alternative));
            }
            continue;
          }
          if (!isa<DbgInfoIntrinsic>(call) && !call->onlyReadsMemory())
            for (const Use &argument : call->args())
              if (argument->getType()->isPointerTy())
                if (parameterOrigin(argument.get(), function, dominators) &&
                    (!callee || !LibraryModels::readsOnly(*callee)))
                  return {}; // Unknown output clobbers require richer
                             // summaries.
          if (call->getType()->isPointerTy())
            state.pointers[call] = pointerFact(call, State{}, nullable);
        } else if (instruction.getType()->isIntegerTy()) {
          state.integers[&instruction] = integerFact(&instruction, state);
        } else if (instruction.getType()->isPointerTy()) {
          state.pointers[&instruction] =
              pointerFact(&instruction, state, nullable);
        }
        if (next.size() + queue.size() >= 512) {
          limit_hit = true;
          return {};
        }
        next.push_back(std::move(current));
      }
      active = std::move(next);
      if (active.empty())
        break;
      if (active.size() + queue.size() > 512) {
        limit_hit = true;
        return {};
      }
    }
    for (Path &current : active) {
      if (const auto *ret =
              dyn_cast<ReturnInst>(current.block->getTerminator())) {
        OutputExit exit;
        if (const ConstantInt *constant =
                integerFact(ret->getReturnValue(), current.state))
          exit.status = signedConstant(*constant);
        exit.writes = std::move(current.writes);
        for (const Argument &argument : function.args())
          if (argument.getType()->isPointerTy()) {
            unsigned bits =
                pointerFact(&argument, current.state, nullable).bits;
            if (bits == Null || bits == NonNull)
              exit.null_requirements[argument.getArgNo()] = bits;
          }
        exits.push_back(std::move(exit));
      } else
        for (const BasicBlock *successor : successors(current.block)) {
          if (queue.size() + exits.size() >= 512) {
            limit_hit = true;
            return {};
          }
          Path alternative = current;
          alternative.predecessor = current.block;
          alternative.block = successor;
          if (const auto *branch =
                  dyn_cast<BranchInst>(current.block->getTerminator()))
            if (branch->isConditional())
              refineCondition(alternative.state, branch->getCondition(),
                              branch->getSuccessor(0) == successor, nullable);
          if (alternative.state.reachable)
            queue.push_back(std::move(alternative));
        }
    }
  }
  return wrote_anything ? exits : std::vector<OutputExit>{};
}

void refine(State &state, const Value *pointer, bool nonnull,
            const NullableFunctions &nullable) {
  NullFact fact = pointerFact(pointer, state, nullable);
  unsigned keep = nonnull ? NonNull : Null;
  if (!(fact.bits & UnknownNull) && !(fact.bits & keep)) {
    state.reachable = false;
    return;
  }
  const Value *identity = fact.identity;
  fact.bits = keep;
  state.pointers[pointer] = fact;
  if (!identity)
    return;
  if (nonnull)
    state.checked_pointers.insert(identity);
  else
    state.checked_pointers.erase(identity);
  for (auto &entry : state.pointers)
    if (entry.second.identity == identity)
      entry.second.bits = keep;
  for (auto &entry : state.cells)
    if (entry.second.pointer.identity == identity)
      entry.second.pointer.bits = keep;
}

void refineCondition(State &state, const Value *condition, bool truth,
                     const NullableFunctions &nullable, unsigned depth) {
  if (depth > 8)
    return;
  if (const auto *constant = integerFact(condition, state)) {
    if (constant->isZero() == truth)
      state.reachable = false;
    return;
  }
  const Value *source = scalarSource(condition, state);
  if (source && source != condition && source->getType()->isIntegerTy(1)) {
    refineCondition(state, source, truth, nullable, depth + 1);
    return;
  }
  auto boolean = state.boolean_guards.find(condition);
  if (boolean != state.boolean_guards.end()) {
    const BooleanGuard &guard = boolean->second;
    if (!(truth ? guard.true_possible : guard.false_possible)) {
      state.reachable = false;
      return;
    }
    for (const auto &entry : truth ? guard.when_true : guard.when_false) {
      ScalarDomain domain =
          intersectDomain(domainFor(entry.first, state), entry.second);
      if (domain.lower > domain.upper ||
          (domain.finite && domain.values.empty()))
        state.reachable = false;
      state.domains[entry.first] = std::move(domain);
      if (const auto *call = dyn_cast<CallBase>(entry.first))
        state.checked_statuses.insert(call);
    }
    updateConditionalCells(state);
    return;
  }
  if (const auto *comparison = dyn_cast<ICmpInst>(condition)) {
    const Value *pointer = nullptr;
    if (isa<ConstantPointerNull>(comparison->getOperand(0)))
      pointer = comparison->getOperand(1);
    else if (isa<ConstantPointerNull>(comparison->getOperand(1)))
      pointer = comparison->getOperand(0);
    if (pointer && comparison->isEquality()) {
      refine(state, pointer,
             truth == (comparison->getPredicate() == ICmpInst::ICMP_NE),
             nullable);
      return;
    }
    const Value *operand = comparison->getOperand(0);
    const ConstantInt *constant = integerFact(comparison->getOperand(1), state);
    auto predicate = comparison->getPredicate();
    if (!constant) {
      constant = integerFact(comparison->getOperand(0), state);
      operand = comparison->getOperand(1);
      predicate = ICmpInst::getSwappedPredicate(predicate);
    }
    if (constant && constant->getBitWidth() <= 64) {
      const Value *boolean = scalarSource(operand, state);
      int64_t value = signedConstant(*constant);
      if (comparison->isEquality() && boolean &&
          boolean->getType()->isIntegerTy(1) && (value == 0 || value == 1) &&
          isa<ICmpInst>(boolean)) {
        bool inner_truth = truth == (predicate == ICmpInst::ICMP_EQ);
        if (value == 0)
          inner_truth = !inner_truth;
        refineCondition(state, boolean, inner_truth, nullable, depth + 1);
      } else {
        if (!truth)
          predicate = ICmpInst::getInversePredicate(predicate);
        if (!refineScalar(state, operand, predicate, value))
          state.unsupported_guard = true;
      }
    } else
      state.unsupported_guard = true;
    return;
  }
  if (const auto *binary = dyn_cast<BinaryOperator>(condition)) {
    if ((binary->getOpcode() == Instruction::And && truth) ||
        (binary->getOpcode() == Instruction::Or && !truth)) {
      refineCondition(state, binary->getOperand(0), truth, nullable, depth + 1);
      refineCondition(state, binary->getOperand(1), truth, nullable, depth + 1);
    } else if (binary->getOpcode() == Instruction::Xor) {
      if (const auto *one = dyn_cast<ConstantInt>(binary->getOperand(1)))
        if (one->isOne())
          refineCondition(state, binary->getOperand(0), !truth, nullable,
                          depth + 1);
    }
    return;
  }
  if (!refineScalar(state, condition,
                    truth ? ICmpInst::ICMP_NE : ICmpInst::ICMP_EQ, 0))
    state.unsupported_guard = true;
}

bool covers(const MemoryRegion &outer, const MemoryRegion &inner) {
  return outer.object == inner.object && outer.offset <= inner.offset &&
         static_cast<uint64_t>(inner.offset - outer.offset) <= outer.bytes &&
         inner.bytes <=
             outer.bytes - static_cast<uint64_t>(inner.offset - outer.offset);
}

Cell memoryCell(const State &state, const MemoryRegion &region) {
  auto exact = state.memory_cells.find(region);
  if (exact != state.memory_cells.end())
    return exact->second;
  for (const auto &entry : state.memory_cells)
    if (covers(entry.first, region))
      return entry.second;
  Cell cell;
  for (const auto &entry : state.memory_cells)
    if (entry.first.object == region.object && entry.first.offset >= 0 &&
        static_cast<uint64_t>(std::max(entry.first.offset, region.offset) -
                              std::min(entry.first.offset, region.offset)) <
            (entry.first.offset <= region.offset ? entry.first.bytes
                                                 : region.bytes))
      return cell; // Partly known bytes do not prove a whole-span state.
  cell.initialization = memoryDefault(state, region);
  return cell;
}

void setMemoryCell(State &state, const MemoryRegion &region, const Cell &cell,
                   const DataLayout &layout) {
  for (auto &entry : state.memory_cells)
    if (!(entry.first == region) && covers(region, entry.first)) {
      if (cell.initialization == Init && cell.pending_outputs.empty()) {
        entry.second = {};
        entry.second.initialization = Init;
      } else if (!cell.pending_outputs.empty()) {
        unsigned prior = entry.second.initialization;
        entry.second = cell;
        entry.second.initialization = prior | Init;
        if (prior == Init)
          entry.second.pending_outputs.clear();
      }
    }
  state.memory_cells[region] = cell;
  if (const auto *alloca = dyn_cast<AllocaInst>(region.object)) {
    if (region.offset == 0 && scalarCell(alloca, layout) &&
        region.bytes == layout.getTypeAllocSize(alloca->getAllocatedType()))
      state.cells[alloca] = cell;
    else if (scalarCell(alloca, layout))
      state.cells[alloca] =
          {}; // Partial writes do not initialize a full scalar.
  }
}

void invalidateMemoryObject(State &state, const Value *object) {
  if (!object)
    return;
  state.root_initialization[object] = UnknownInit;
  for (auto &entry : state.memory_cells)
    if (entry.first.object == object)
      entry.second = {};
  if (const auto *alloca = dyn_cast<AllocaInst>(object)) {
    auto cell = state.cells.find(alloca);
    if (cell != state.cells.end()) {
      cell->second = {};
      cell->second.escaped = true;
    }
  }
}

void invalidateAliasedMemory(State &state) {
  std::set<const Value *> objects = state.aliased_objects;
  for (const auto &entry : state.cells)
    if (entry.second.escaped)
      objects.insert(entry.first);
  for (const auto &entry : state.memory_cells)
    if (isa<GlobalVariable>(entry.first.object))
      objects.insert(entry.first.object);
  for (const Value *object : objects)
    invalidateMemoryObject(state, object);
}

bool comparisonUse(const Value *value, unsigned depth = 0);

bool scanfOutputComparedAfter(const CallBase &call,
                              const std::vector<ScanfOutput> &outputs,
                              const State &state, const DataLayout &layout) {
  std::vector<MemoryRegion> regions;
  for (const auto &output : outputs) {
    const Value *pointer = call.getArgOperand(output.argument);
    const auto *type = dyn_cast<PointerType>(pointer->getType());
    if (type && !type->isOpaque())
      if (auto region = memoryRegion(pointer, type->getPointerElementType(),
                                     state, layout))
        regions.push_back(*region);
  }
  for (const BasicBlock &block : *call.getFunction())
    for (const Instruction &instruction : block)
      if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
        if (!comparisonUse(load) || !isPotentiallyReachable(&call, load))
          continue;
        if (auto region = memoryRegion(load->getPointerOperand(),
                                       load->getType(), state, layout))
          if (std::any_of(regions.begin(), regions.end(),
                          [&](const MemoryRegion &output) {
                            return covers(output, *region) ||
                                   covers(*region, output);
                          }))
            return true;
      }
  return false;
}

bool applyOutputEffects(const CallBase &call, State &state,
                        const OutputSummaries &summaries,
                        const NullableFunctions &nullable,
                        const DataLayout &layout) {
  const Function *callee = ValueFacts::callee(call);
  if (!callee)
    return false;
  auto scanf = scanfOutputs(call, state);
  if (!scanf.empty()) {
    state.domains[&call] = domainFor(&call, state);
    if (auto model = LibraryModels::format(*callee))
      if (model->format < call.arg_size())
        if (auto literal =
                trackedString(*call.getArgOperand(model->format), state))
          if (auto parsed = parseFormat(*literal, true)) {
            int64_t maximum_count = 0;
            for (const auto &conversion : parsed->conversions)
              maximum_count += conversion.conversion != 'n';
            state.domains[&call].upper = maximum_count;
          }
    if (ValueFacts::macroDefined(*call.getModule(),
                                 "_LINUX_KERNEL_SPRINTF_H_") ||
        ValueFacts::macroDefined(*call.getModule(), "_LINUX_KERNEL_H"))
      state.domains[&call].lower = 0;
    bool incorrect = incorrectlyCheckedScanf(call);
    bool compared_output = scanfOutputComparedAfter(call, scanf, state, layout);
    for (const ScanfOutput &output : scanf) {
      const Value *pointer = call.getArgOperand(output.argument);
      auto *type = dyn_cast<PointerType>(pointer->getType());
      if (!type || type->isOpaque())
        continue;
      auto region = output.bytes
                        ? memoryRegion(pointer, output.bytes, state, layout)
                        : memoryRegion(pointer, type->getPointerElementType(),
                                       state, layout);
      if (!region) {
        if (auto object = memoryRegion(pointer, uint64_t(1), state, layout))
          invalidateMemoryObject(state, object->object);
        continue;
      }
      Cell cell = memoryCell(state, *region);
      PendingOutput pending;
      pending.call = &call;
      pending.minimum_count = output.minimum_count;
      pending.prior_initialization = cell.initialization;
      pending.incorrectly_checked_scanf = incorrect;
      pending.output_compared_after_call = compared_output;
      pending.return_count_maps_output = output.count_maps_output;
      auto previous = cell.pending_outputs.find(&call);
      if (previous != cell.pending_outputs.end())
        pending.minimum_count =
            std::min(pending.minimum_count, previous->second.minimum_count);
      cell.pending_outputs[&call] = std::move(pending);
      cell.initialization |= Init;
      if (output.always_written) {
        cell.initialization = Init;
        cell.pending_outputs.clear();
      }
      cell.integer = nullptr;
      cell.pointer = {};
      cell.scalar_source = nullptr;
      cell.stored_pointer = nullptr;
      cell.last_write = &call;
      setMemoryCell(state, *region, cell, layout);
    }
    return true;
  }
  auto summary = summaries.find(callee);
  if (summary == summaries.end())
    return false;
  std::vector<const OutputExit *> exits;
  std::set<OutputWrite> writes;
  ScalarDomain domain = domainFor(&call, state);
  domain.finite = true;
  for (const OutputExit &exit : summary->second)
    if (compatibleExit(exit, call, state, nullable)) {
      exits.push_back(&exit);
      writes.insert(exit.writes.begin(), exit.writes.end());
      if (exit.status)
        domain.values.insert(*exit.status);
      else
        domain.finite = false;
    }
  if (exits.empty())
    return false;
  state.domains[&call] = std::move(domain);
  for (const OutputWrite &write : writes) {
    if (write.argument >= call.arg_size())
      continue;
    auto region = memoryRegion(call.getArgOperand(write.argument), write.bytes,
                               state, layout);
    if (!region ||
        write.offset > std::numeric_limits<int64_t>::max() - region->offset)
      continue;
    region->offset += write.offset;
    if (auto extent = objectExtent(*region->object))
      if (static_cast<uint64_t>(region->offset) > *extent ||
          region->bytes > *extent - static_cast<uint64_t>(region->offset)) {
        invalidateMemoryObject(state, region->object);
        continue;
      }
    Cell cell = memoryCell(state, *region);
    PendingOutput pending;
    pending.call = &call;
    pending.kind = ConditionalOutputKind::DefinedInitializer;
    pending.prior_initialization = cell.initialization;
    bool always = true;
    for (const OutputExit *exit : exits) {
      bool written = std::any_of(
          exit->writes.begin(), exit->writes.end(),
          [&](const OutputWrite &effect) {
            return effect.argument == write.argument &&
                   effect.offset <= write.offset &&
                   static_cast<uint64_t>(write.offset - effect.offset) <=
                       effect.bytes &&
                   write.bytes <=
                       effect.bytes -
                           static_cast<uint64_t>(write.offset - effect.offset);
          });
      always &= written;
      if (exit->status)
        (written ? pending.written_statuses : pending.unwritten_statuses)
            .insert(*exit->status);
      else
        pending.unknown_status = true;
    }
    cell.integer = nullptr;
    cell.pointer = {};
    cell.scalar_source = nullptr;
    cell.stored_pointer = nullptr;
    cell.last_write = &call;
    if (always) {
      cell.initialization = Init;
      cell.pending_outputs.clear();
    } else if ((cell.initialization & Uninit) &&
               !(cell.initialization & UnknownInit)) {
      cell.initialization |= Init;
      cell.pending_outputs[&call] = std::move(pending);
    }
    setMemoryCell(state, *region, cell, layout);
  }
  return true;
}

bool comparisonUse(const Value *value, unsigned depth) {
  if (!value || depth > 12)
    return true;
  for (const User *user : value->users()) {
    if (isa<ICmpInst>(user) || isa<FCmpInst>(user))
      return true;
    if (isa<CastInst>(user) && comparisonUse(user, depth + 1))
      return true;
  }
  return false;
}

bool assignmentOrReturnUse(const Value *value, unsigned depth = 0) {
  if (!value || depth > 12)
    return false;
  for (const User *user : value->users()) {
    if (isa<ReturnInst>(user))
      return true;
    if (const auto *store = dyn_cast<StoreInst>(user))
      if (store->getValueOperand() == value)
        return true;
    if ((isa<CastInst>(user) || isa<BinaryOperator>(user)) &&
        assignmentOrReturnUse(user, depth + 1))
      return true;
  }
  return false;
}

PointerNullState publicNull(unsigned bits) {
  if (bits == Null)
    return PointerNullState::Null;
  if (bits == NonNull)
    return PointerNullState::NonNull;
  if (bits == (Null | NonNull))
    return PointerNullState::Nullable;
  return PointerNullState::Unknown;
}

InitializationState publicInit(unsigned bits) {
  if (bits == Uninit)
    return InitializationState::Uninitialized;
  if (bits == Init)
    return InitializationState::Initialized;
  if (bits == (Uninit | Init))
    return InitializationState::MaybeUninitialized;
  return InitializationState::Unknown;
}

void addFinding(StateAnalysisResult &result, const std::set<std::string> &selected,
                const std::string &id, const std::string &message,
                const Instruction &site,
                const std::set<const Instruction *> &evidence) {
  if (!selected.empty() && !selected.count(id))
    return;
  for (AnalysisFinding &existing : result.findings)
    if (existing.rule_id == id && existing.site == &site) {
      for (const Instruction *instruction : evidence)
        if (std::find(existing.evidence.begin(), existing.evidence.end(),
                      instruction) == existing.evidence.end())
          existing.evidence.push_back(instruction);
      return;
    }
  AnalysisFinding finding{id, &site, message, {}};
  finding.evidence.assign(evidence.begin(), evidence.end());
  result.findings.push_back(std::move(finding));
}

void observe(const Instruction &instruction, const State &state,
             const DataLayout &layout, const NullableFunctions &nullable,
             StateAnalysisResult &result) {
  std::set<const Value *> operands;
  for (const Use &operand : instruction.operands())
    if (operand->getType()->isPointerTy() && !isa<Function>(operand.get()))
      operands.insert(operand.get());
  for (const Value *pointer : operands) {
    PointerStateFact fact;
    fact.pointer = pointer;
    fact.nullness = publicNull(pointerFact(pointer, state, nullable).bits);
    if (const AllocaInst *cell = scalarCell(pointer, layout)) {
      auto found = state.cells.find(cell);
      if (found != state.cells.end())
        fact.pointee_initialization = publicInit(found->second.initialization);
    }
    Type *access_type = nullptr;
    if (const auto *load = dyn_cast<LoadInst>(&instruction))
      if (load->getPointerOperand() == pointer)
        access_type = load->getType();
    if (const auto *store = dyn_cast<StoreInst>(&instruction))
      if (store->getPointerOperand() == pointer)
        access_type = store->getValueOperand()->getType();
    if (access_type)
      if (auto region = memoryRegion(pointer, access_type, state, layout))
        fact.pointee_initialization =
            publicInit(memoryCell(state, *region).initialization);
    result.states[&instruction].push_back(fact);
  }
}

// For cross-call diagnostics require a retained parameter dereference on every
// normal return, rather than assuming that an arbitrary callee reads a pointer.
ParameterReads summarizeParameterReads(const Module &module) {
  ParameterReads result;
  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    DominatorTree dominators(const_cast<Function &>(function));
    std::vector<const ReturnInst *> returns;
    for (const BasicBlock &block : function)
      if (const auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
        if (dominators.isReachableFromEntry(&block))
          returns.push_back(ret);
    if (returns.empty())
      continue;
    for (const BasicBlock &block : function)
      for (const Instruction &instruction : block) {
        const Value *pointer = nullptr;
        if (const auto *load = dyn_cast<LoadInst>(&instruction))
          pointer = load->getPointerOperand();
        if (const auto *store = dyn_cast<StoreInst>(&instruction))
          pointer = store->getPointerOperand();
        if (!pointer)
          continue;
        const auto *argument = parameterOrigin(pointer, function, dominators);
        if (!argument || argument->getParent() != &function)
          continue;
        if (std::all_of(returns.begin(), returns.end(),
                        [&](const ReturnInst *ret) {
                          return dominators.dominates(&instruction, ret);
                        }))
          result[&function].emplace(argument->getArgNo(), &instruction);
      }
  }
  return result;
}

struct FunctionRun {
  bool returns_null = false;
  bool converged = true;
  std::map<const Value *, std::vector<const Instruction *>>
      guarded_dereferences;
  std::map<const Value *, std::vector<const Instruction *>>
      unguarded_dereferences;
  std::set<const CallBase *> checked_pointer_calls;
  std::set<const CallBase *> dereferenced_pointer_calls;
};

Optional<unsigned> sizeArgument(const CallBase &call) {
  if (isa<MemIntrinsic>(call))
    return 2;
  const Function *callee = ValueFacts::callee(call);
  if (!callee)
    return None;
  for (const char *name :
       {"write", "read", "memmove", "memset", "memcpy", "memcmp", "strncat",
        "strncpy", "strncmp", "strndup"})
    if (ValueFacts::hasLibraryName(*callee, name, true))
      return 2;
  if (ValueFacts::hasLibraryName(*callee, "lseek", true) ||
      ValueFacts::hasLibraryName(*callee, "snprintf", true))
    return 1;
  return None;
}

FunctionRun analyzeFunction(const Function &function,
                            const NullableFunctions &nullable,
                            const ParameterReads &reads,
                            const OutputSummaries &output_summaries,
                            const std::set<std::string> &selected,
                            StateAnalysisResult *result) {
  const DataLayout &layout = function.getParent()->getDataLayout();
  std::map<const BasicBlock *, State> inputs;
  std::map<const BasicBlock *, State> outputs;
  std::map<std::pair<const BasicBlock *, const BasicBlock *>, State> edges;
  std::deque<const BasicBlock *> queue;
  std::set<const BasicBlock *> queued;
  State entry;
  entry.reachable = true;
  for (const Argument &argument : function.args())
    if (argument.getType()->isPointerTy())
      entry.pointers[&argument] = {
          argument.hasNonNullAttr() ? NonNull : UnknownNull, &argument, {}};
    else if (argument.getType()->isIntegerTy())
      entry.scalar_sources[&argument] = &argument;
  inputs[&function.getEntryBlock()] = entry;
  queue.push_back(&function.getEntryBlock());
  queued.insert(&function.getEntryBlock());
  const size_t limit = std::max<size_t>(1024, function.size() * 128);
  size_t visits = 0;
  FunctionRun run;
  DominatorTree dominators(const_cast<Function &>(function));
  LoopInfo function_loops(dominators);
  std::vector<const LoadInst *> mixed_candidates;
  struct ArgumentUse {
    const CallBase *call;
    const Value *source;
  };
  struct GuardUse {
    const BranchInst *branch;
    const Value *source;
    bool relational_zero;
  };
  std::vector<ArgumentUse> argument_uses;
  std::vector<GuardUse> guard_uses;

  auto transfer = [&](const BasicBlock &block, State state, bool emit,
                      const BasicBlock *predecessor = nullptr,
                      const Instruction *stop_before = nullptr) {
    if (!state.reachable)
      return state;
    for (const Instruction &instruction : block) {
      if (&instruction == stop_before)
        return state;
      if (const auto *phi = dyn_cast<PHINode>(&instruction)) {
        if (predecessor) {
          int index = phi->getBasicBlockIndex(predecessor);
          if (index >= 0) {
            const Value *incoming = phi->getIncomingValue(index);
            if (phi->getType()->isIntegerTy()) {
              state.integers[phi] = integerFact(incoming, state);
              state.scalar_sources[phi] = scalarSource(incoming, state);
            } else if (phi->getType()->isPointerTy()) {
              state.pointers[phi] = pointerFact(incoming, state, nullable);
              auto copy = state.pointer_copies.find(incoming);
              state.pointer_copies[phi] =
                  copy == state.pointer_copies.end() ? incoming : copy->second;
            }
          }
          continue;
        }
        if (phi->getType()->isIntegerTy()) {
          const ConstantInt *constant = nullptr;
          const Value *source = nullptr;
          bool first = true;
          for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            auto edge = edges.find({phi->getIncomingBlock(i), &block});
            if (edge == edges.end() || !edge->second.reachable)
              continue;
            const ConstantInt *incoming =
                integerFact(phi->getIncomingValue(i), edge->second);
            const Value *incoming_source =
                scalarSource(phi->getIncomingValue(i), edge->second);
            if (first) {
              constant = incoming;
              source = incoming_source;
            } else {
              if (constant != incoming)
                constant = nullptr;
              if (source != incoming_source)
                source = nullptr;
            }
            first = false;
          }
          state.integers[phi] = constant;
          state.scalar_sources[phi] = source ? source : phi;
          if (phi->getType()->isIntegerTy(1)) {
            BooleanGuard guard;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
              auto edge = edges.find({phi->getIncomingBlock(i), &block});
              if (edge == edges.end() || !edge->second.reachable)
                continue;
              for (bool truth : {false, true}) {
                State alternative = edge->second;
                refineCondition(alternative, phi->getIncomingValue(i), truth,
                                nullable);
                if (!alternative.reachable)
                  continue;
                bool &possible =
                    truth ? guard.true_possible : guard.false_possible;
                auto &implied = truth ? guard.when_true : guard.when_false;
                if (!possible)
                  implied = alternative.domains;
                else
                  mergeDomains(implied, alternative.domains);
                possible = true;
              }
            }
            state.boolean_guards[phi] = std::move(guard);
          }
        }
        if (!phi->getType()->isPointerTy())
          continue;
        NullFact fact;
        bool first = true;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
          auto edge = edges.find({phi->getIncomingBlock(i), &block});
          if (edge == edges.end() || !edge->second.reachable)
            continue;
          NullFact incoming =
              pointerFact(phi->getIncomingValue(i), edge->second, nullable);
          fact = first ? incoming : mergeFact(fact, incoming);
          first = false;
        }
        if (!first) {
          if (!fact.identity)
            fact.identity = phi;
          state.pointers[phi] = fact;
        }
        continue;
      }
      if (emit)
        observe(instruction, state, layout, nullable, *result);
      if (emit)
        if (const auto *branch = dyn_cast<BranchInst>(&instruction))
          if (branch->isConditional()) {
            const Value *condition =
                scalarSource(branch->getCondition(), state);
            if (const auto *comparison =
                    dyn_cast_or_null<ICmpInst>(condition)) {
              const Value *operand = comparison->getOperand(0);
              const ConstantInt *constant =
                  integerFact(comparison->getOperand(1), state);
              if (!constant) {
                constant = integerFact(comparison->getOperand(0), state);
                operand = comparison->getOperand(1);
              }
              const Value *source = scalarSource(operand, state);
              if (source && constant)
                guard_uses.push_back(
                    {branch, source,
                     !comparison->isEquality() && constant->isZero()});
            } else if (condition)
              guard_uses.push_back({branch, condition, false});
          }
      if (emit)
        if (const auto *comparison = dyn_cast<ICmpInst>(&instruction)) {
          const Value *pointer = nullptr;
          if (isa<ConstantPointerNull>(comparison->getOperand(0)))
            pointer = comparison->getOperand(1);
          if (isa<ConstantPointerNull>(comparison->getOperand(1)))
            pointer = comparison->getOperand(0);
          if (pointer && comparison->isEquality()) {
            NullFact fact = pointerFact(pointer, state, nullable);
            if (const auto *call = dyn_cast_or_null<CallBase>(fact.identity))
              run.checked_pointer_calls.insert(call);
          }
        }
      if (const auto *alloca = dyn_cast<AllocaInst>(&instruction)) {
        for (auto it = state.memory_cells.begin();
             it != state.memory_cells.end();)
          if (it->first.object == alloca)
            it = state.memory_cells.erase(it);
          else
            ++it;
        if (scalarCell(alloca, layout))
          state.cells[alloca] = {Uninit, {}, alloca};
        state.pointers[alloca] = {NonNull, alloca, {}};
        state.root_initialization[alloca] = Uninit;
        continue;
      }
      const Value *dereference = nullptr;
      if (const auto *load = dyn_cast<LoadInst>(&instruction))
        dereference = load->getPointerOperand();
      if (const auto *store = dyn_cast<StoreInst>(&instruction))
        dereference = store->getPointerOperand();
      if (dereference && emit) {
        NullFact fact = pointerFact(dereference, state, nullable);
        if (fact.identity) {
          const Value *definition =
              fact.definition ? fact.definition : fact.identity;
          if (!fact.ambiguous_definition) {
            if (state.checked_pointers.count(fact.identity))
              run.guarded_dereferences[definition].push_back(&instruction);
            else if (fact.bits != NonNull)
              run.unguarded_dereferences[definition].push_back(&instruction);
          }
          if (const auto *call = dyn_cast<CallBase>(fact.identity))
            run.dereferenced_pointer_calls.insert(call);
        }
        if (isNullable(fact))
          addFinding(*result, selected, "cpp/missing-null-test",
                     "Pointer may be null on a reaching CFG edge before this "
                     "dereference.",
                     instruction, fact.origins);
      }
      if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
        auto region = memoryRegion(load->getPointerOperand(), load->getType(),
                                   state, layout);
        Cell output_cell;
        if (region)
          output_cell = memoryCell(state, *region);
        if (emit && !load->use_empty() &&
            (output_cell.initialization & Uninit) &&
            !(output_cell.initialization & UnknownInit))
          for (const auto &entry : output_cell.pending_outputs) {
            const PendingOutput &pending = entry.second;
            if (outputGuaranteed(pending, domainFor(pending.call, state)))
              continue;
            bool first_read = !region || !state.reported_outputs.count(
                                             {pending.call, *region});
            if (first_read && pending.kind == ConditionalOutputKind::Scanf) {
              if (!pending.incorrectly_checked_scanf)
                addFinding(*result, selected, "cpp/missing-check-scanf",
                           "Scanf output is read without proving that the "
                           "return count is at least " +
                               std::to_string(pending.minimum_count) + ".",
                           instruction, {pending.call});
              if (pending.call->use_empty() &&
                  !pending.output_compared_after_call &&
                  assignmentOrReturnUse(load))
                addFinding(*result, selected,
                           "cpp/improper-check-return-value-scanf",
                           "Ignored scanf status leaves an uninitialized "
                           "output used in an assignment or return.",
                           *pending.call, {load});
            } else if (first_read &&
                       pending.kind ==
                           ConditionalOutputKind::DefinedInitializer &&
                       !state.checked_statuses.count(pending.call)) {
              addFinding(
                  *result, selected, "cpp/conditionally-uninitialized-variable",
                  "This initializer may return without writing its output, "
                  "which is read before checking the returned status.",
                  *pending.call, {load});
            }
          }
        if (!load->use_empty() && region &&
            (output_cell.initialization & Uninit) &&
            !(output_cell.initialization & UnknownInit))
          for (const auto &entry : output_cell.pending_outputs)
            if (!outputGuaranteed(entry.second, domainFor(entry.first, state)))
              state.reported_outputs.insert({entry.first, *region});
        const AllocaInst *cell =
            scalarCell(load->getPointerOperand(), layout, load->getType());
        auto found = state.cells.find(cell);
        if (cell && found != state.cells.end()) {
          if (emit && !load->use_empty() &&
              found->second.pending_outputs.empty() &&
              found->second.initialization == Uninit)
            addFinding(*result, selected, "cpp/uninitialized-local",
                       "Scalar stack cell is uninitialized before this read on "
                       "all tracked predecessors.",
                       instruction, {cell});
          else if (emit && !load->use_empty() &&
                   found->second.pending_outputs.empty() &&
                   found->second.initialization == (Uninit | Init))
            mixed_candidates.push_back(load);
          if (load->getType()->isPointerTy())
            state.pointers[load] = found->second.pointer;
          if (load->getType()->isIntegerTy())
            state.integers[load] = found->second.integer;
          if (load->getType()->isIntegerTy())
            state.scalar_sources[load] = found->second.scalar_source;
          if (load->getType()->isPointerTy())
            state.pointer_copies[load] = found->second.stored_pointer;
        } else if (load->getType()->isPointerTy()) {
          state.pointers[load] = {};
        } else if (load->getType()->isIntegerTy()) {
          state.integers[load] = nullptr;
          state.scalar_sources[load] = load;
        }
        continue;
      }
      if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
        if (const AllocaInst *cell =
                scalarCell(store->getPointerOperand(), layout,
                           store->getValueOperand()->getType())) {
          Cell &value = state.cells[cell];
          value.initialization =
              isa<UndefValue>(store->getValueOperand()) ||
                      isa<PoisonValue>(store->getValueOperand())
                  ? Uninit
                  : Init;
          value.pointer =
              pointerFact(store->getValueOperand(), state, nullable);
          value.pointer.definition = store;
          value.pointer.ambiguous_definition = false;
          if (value.pointer.bits == Null)
            value.pointer.origins.insert(store);
          value.last_write = store;
          value.integer = integerFact(store->getValueOperand(), state);
          value.scalar_source = scalarSource(store->getValueOperand(), state);
          value.stored_pointer =
              store->getValueOperand()->getType()->isPointerTy()
                  ? store->getValueOperand()
                  : nullptr;
          value.pending_outputs.clear();
        } else if (const auto *root = dyn_cast<AllocaInst>(
                       getUnderlyingObject(store->getPointerOperand()))) {
          auto found = state.cells.find(root);
          if (found != state.cells.end()) {
            bool escaped = found->second.escaped;
            found->second = {};
            found->second.escaped = escaped;
          }
        } else {
          // An unresolved indirect store may update any cell whose address
          // has been copied into another pointer or escaped the function.
          invalidateAliasedMemory(state);
        }
        if (auto region = memoryRegion(store->getPointerOperand(),
                                       store->getValueOperand()->getType(),
                                       state, layout)) {
          Cell value;
          value.initialization =
              isa<UndefValue>(store->getValueOperand()) ||
                      isa<PoisonValue>(store->getValueOperand())
                  ? Uninit
                  : Init;
          value.pointer =
              pointerFact(store->getValueOperand(), state, nullable);
          value.pointer.definition = store;
          value.pointer.ambiguous_definition = false;
          value.integer = integerFact(store->getValueOperand(), state);
          value.scalar_source = scalarSource(store->getValueOperand(), state);
          value.stored_pointer =
              store->getValueOperand()->getType()->isPointerTy()
                  ? store->getValueOperand()
                  : nullptr;
          value.last_write = store;
          if (value.pointer.bits == Null)
            value.pointer.origins.insert(store);
          if (const auto *alloca = dyn_cast<AllocaInst>(region->object)) {
            auto prior = state.cells.find(alloca);
            if (prior != state.cells.end())
              value.escaped = prior->second.escaped;
          }
          for (auto &existing : state.memory_cells)
            if (existing.first.object == region->object &&
                existing.first.offset >= 0 &&
                (covers(existing.first, *region) ||
                 covers(*region, existing.first))) {
              if (covers(*region, existing.first)) {
                existing.second = {};
                existing.second.initialization = value.initialization;
              } else
                existing.second = {};
            }
          setMemoryCell(state, *region, value, layout);
        }
        // An address held in another local pointer also introduces aliases;
        // unresolved writes must not leave a definite uninitialized fact.
        if (store->getValueOperand()->getType()->isPointerTy()) {
          if (auto region = memoryRegion(store->getValueOperand(), uint64_t(1),
                                         state, layout))
            state.aliased_objects.insert(region->object);
          const auto *root = dyn_cast<AllocaInst>(
              getUnderlyingObject(store->getValueOperand()));
          auto found = state.cells.find(root);
          if (found != state.cells.end())
            found->second.escaped = true;
        }
        continue;
      }
      if (const auto *call = dyn_cast<CallBase>(&instruction)) {
        state.domains.erase(call);
        state.checked_statuses.erase(call);
        state.checked_pointers.erase(call);
        // A lexical call may execute again in a loop. Previous writes from
        // that dynamic execution cannot be validated by this new return code.
        for (auto &entry : state.cells)
          entry.second.pending_outputs.erase(call);
        for (auto &entry : state.memory_cells)
          entry.second.pending_outputs.erase(call);
        for (auto it = state.reported_outputs.begin();
             it != state.reported_outputs.end();)
          if (it->first == call)
            it = state.reported_outputs.erase(it);
          else
            ++it;
        if (const auto *assume = dyn_cast<IntrinsicInst>(call))
          if (assume->getIntrinsicID() == Intrinsic::assume) {
            refineCondition(state, assume->getArgOperand(0), true, nullable);
            if (!state.reachable)
              return state;
            continue;
          }
        if (const auto *variadic = dyn_cast<IntrinsicInst>(call))
          if (variadic->getIntrinsicID() == Intrinsic::vastart ||
              variadic->getIntrinsicID() == Intrinsic::vacopy) {
            const auto *root = dyn_cast<AllocaInst>(
                getUnderlyingObject(variadic->getArgOperand(0)));
            auto found = state.cells.find(root);
            if (found != state.cells.end()) {
              found->second.initialization = Init;
              found->second.pointer = {};
              found->second.last_write = variadic;
            }
            continue;
          }
        if (const auto *lifetime = dyn_cast<IntrinsicInst>(call))
          if (lifetime->getIntrinsicID() == Intrinsic::lifetime_start ||
              lifetime->getIntrinsicID() == Intrinsic::lifetime_end) {
            const auto *root = dyn_cast<AllocaInst>(
                getUnderlyingObject(lifetime->getArgOperand(1)));
            auto found = state.cells.find(root);
            if (found != state.cells.end()) {
              found->second.initialization =
                  lifetime->getIntrinsicID() == Intrinsic::lifetime_start
                      ? Uninit
                      : UnknownInit;
              found->second.pointer = {};
              found->second.last_write = lifetime;
            }
            if (root) {
              state.root_initialization[root] =
                  lifetime->getIntrinsicID() == Intrinsic::lifetime_start
                      ? Uninit
                      : UnknownInit;
              for (auto it = state.memory_cells.begin();
                   it != state.memory_cells.end();)
                if (it->first.object == root)
                  it = state.memory_cells.erase(it);
                else
                  ++it;
            }
            continue;
          }
        const Function *callee = ValueFacts::callee(*call);
        if (emit)
          if (auto position = sizeArgument(*call))
            if (*position < call->arg_size()) {
              const Value *source =
                  scalarSource(call->getArgOperand(*position), state);
              if (source && !isa<ConstantInt>(source))
                argument_uses.push_back({call, source});
            }
        bool modeled_outputs = applyOutputEffects(
            *call, state, output_summaries, nullable, layout);
        if (emit)
          for (unsigned argument : memoryArguments(*call)) {
            NullFact fact =
                pointerFact(call->getArgOperand(argument), state, nullable);
            if (isNullable(fact))
              addFinding(*result, selected, "cpp/missing-null-test",
                         "A nonzero memory transfer dereferences a possibly "
                         "null buffer.",
                         instruction, fact.origins);
          }
        if (emit && callee) {
          auto summary = reads.find(callee);
          if (summary != reads.end())
            for (const auto &parameter : summary->second)
              if (parameter.first < call->arg_size()) {
                NullFact fact = pointerFact(
                    call->getArgOperand(parameter.first), state, nullable);
                // This rule specifically concerns a result of a nullable call.
                if (isNullable(fact) && hasCallOrigin(fact)) {
                  fact.origins.insert(parameter.second);
                  addFinding(*result, selected, "cpp/deref-null-result",
                             "Nullable function result reaches a parameter "
                             "dereferenced on every normal callee return.",
                             instruction, fact.origins);
                } else if (isNullable(fact)) {
                  fact.origins.insert(parameter.second);
                  addFinding(*result, selected, "cpp/missing-null-test",
                             "A possibly null argument reaches an "
                             "unconditional callee dereference.",
                             instruction, fact.origins);
                }
              }
        }
        if (const auto *memory = dyn_cast<MemIntrinsic>(call)) {
          const auto *root =
              dyn_cast<AllocaInst>(getUnderlyingObject(memory->getRawDest()));
          auto found = state.cells.find(root);
          if (found != state.cells.end()) {
            bool escaped = found->second.escaped;
            const auto *length = dyn_cast<ConstantInt>(memory->getLength());
            const AllocaInst *exact = scalarCell(memory->getRawDest(), layout);
            if (length && exact && length->getValue().getActiveBits() <= 64 &&
                length->getZExtValue() ==
                    layout.getTypeAllocSize(exact->getAllocatedType())) {
              found->second = {Init, {}, memory};
              if (const auto *set = dyn_cast<MemSetInst>(memory))
                if (const auto *byte = dyn_cast<ConstantInt>(set->getValue()))
                  if (byte->isZero() &&
                      exact->getAllocatedType()->isPointerTy())
                    found->second.pointer = {Null, set, {set}};
              if (const auto *copy = dyn_cast<MemTransferInst>(memory)) {
                const auto *source = scalarCell(copy->getRawSource(), layout);
                auto source_cell = state.cells.find(source);
                if (source_cell != state.cells.end())
                  found->second = source_cell->second;
              }
            } else
              found->second = {};
            found->second.escaped |= escaped;
          }
        } else if (!modeled_outputs && !isa<DbgInfoIntrinsic>(call) &&
                   (!callee || !callee->isIntrinsic()) &&
                   (!callee || !LibraryModels::readsOnly(*callee)) &&
                   !call->onlyReadsMemory()) {
          // Passing a tracked cell to an unknown writer invalidates it. This
          // suppresses unsupported output-parameter and alias assumptions.
          invalidateAliasedMemory(state);
          for (const Use &argument : call->args())
            if (argument->getType()->isPointerTy()) {
              if (auto region =
                      memoryRegion(argument.get(), uint64_t(1), state, layout))
                invalidateMemoryObject(state, region->object);
              const auto *root =
                  dyn_cast<AllocaInst>(getUnderlyingObject(argument.get()));
              auto found = state.cells.find(root);
              if (found != state.cells.end()) {
                found->second = {};
                found->second.escaped = true;
              }
            }
        }
        if (call->getType()->isPointerTy())
          state.pointers[call] = pointerFact(call, State{}, nullable);
        if (call->getType()->isPointerTy() && callee &&
            LibraryModels::allocation(*callee) != AllocationKind::Unknown) {
          unsigned initial = Uninit;
          if (ValueFacts::hasLibraryName(*callee, "calloc", true) ||
              ValueFacts::hasLibraryName(*callee, "strdup", true) ||
              ValueFacts::hasLibraryName(*callee, "strndup", true))
            initial = Init;
          if (ValueFacts::hasLibraryName(*callee, "realloc", true))
            initial = UnknownInit;
          state.root_initialization[call] =
              function_loops.getLoopFor(call->getParent()) ? UnknownInit
                                                           : initial;
        }
        if (call->getType()->isPointerTy() &&
            function_loops.getLoopFor(call->getParent()))
          for (auto &memory : state.memory_cells)
            if (memory.first.object == call)
              memory.second = {};
        if (call->getType()->isIntegerTy()) {
          state.integers[call] = nullptr;
          state.scalar_sources[call] = call;
          if (const auto *intrinsic = dyn_cast<IntrinsicInst>(call))
            if (intrinsic->getIntrinsicID() == Intrinsic::expect)
              state.scalar_sources[call] =
                  scalarSource(call->getArgOperand(0), state);
        }
        continue;
      }
      if (instruction.getType()->isPointerTy()) {
        state.pointers.erase(&instruction);
        state.pointers[&instruction] =
            pointerFact(&instruction, state, nullable);
      }
      if (instruction.getType()->isIntegerTy()) {
        state.integers.erase(&instruction);
        state.integers[&instruction] = integerFact(&instruction, state);
        state.scalar_sources[&instruction] = &instruction;
      }
    }
    return state;
  };

  while (!queue.empty()) {
    if (++visits > limit) {
      run.converged = false;
      break;
    }
    const BasicBlock *block = queue.front();
    queue.pop_front();
    queued.erase(block);
    State state = transfer(*block, inputs[block], false);
    outputs[block] = state;
    for (const BasicBlock *successor : successors(block)) {
      State edge_state = state;
      if (const auto *branch = dyn_cast<BranchInst>(block->getTerminator()))
        if (branch->isConditional())
          refineCondition(edge_state, branch->getCondition(),
                          branch->getSuccessor(0) == successor, nullable);
      if (const auto *selection =
              dyn_cast<SwitchInst>(block->getTerminator())) {
        bool selected_case = false;
        ScalarDomain alternatives;
        alternatives.finite = true;
        const Value *source =
            scalarSource(selection->getCondition(), edge_state);
        for (const auto &case_value : selection->cases())
          if (case_value.getCaseSuccessor() == successor) {
            alternatives.values.insert(
                signedConstant(*case_value.getCaseValue()));
            selected_case = true;
          }
        if (selected_case && source) {
          ScalarDomain prior = domainFor(source, edge_state);
          for (auto it = alternatives.values.begin();
               it != alternatives.values.end();)
            if (!contains(prior, *it))
              it = alternatives.values.erase(it);
            else
              ++it;
          edge_state.domains[source] = alternatives;
          edge_state.reachable &= !alternatives.values.empty();
          if (const auto *call = dyn_cast<CallBase>(source))
            edge_state.checked_statuses.insert(call);
          updateConditionalCells(edge_state);
        } else if (source) {
          ScalarDomain domain = domainFor(source, edge_state);
          for (const auto &case_value : selection->cases())
            domain.excluded.insert(signedConstant(*case_value.getCaseValue()));
          edge_state.domains[source] = domain;
          if (const auto *call = dyn_cast<CallBase>(source))
            edge_state.checked_statuses.insert(call);
          updateConditionalCells(edge_state);
        }
      }
      auto key = std::make_pair(block, successor);
      if (edges.count(key) && edges[key] == edge_state)
        continue;
      edges[key] = edge_state;
      State incoming;
      if (successor == &function.getEntryBlock())
        incoming = entry;
      for (const BasicBlock *predecessor : predecessors(successor)) {
        auto edge = edges.find({predecessor, successor});
        if (edge != edges.end())
          mergeState(incoming, edge->second);
      }
      // PHIs use individual predecessor-edge states. Their inputs can change
      // even when the merged block state is identical (e.g. a new true/false
      // incoming value), so every changed edge must schedule the successor.
      inputs[successor] = std::move(incoming);
      if (queued.insert(successor).second)
        queue.push_back(successor);
    }
  }
  // Emit only after convergence. Transient worklist states are not findings.
  if (run.converged)
    for (const BasicBlock &block : function) {
      auto input = inputs.find(&block);
      if (input == inputs.end() || !input->second.reachable)
        continue;
      State final = transfer(block, input->second, result != nullptr);
      if (const auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
        if (ret->getReturnValue() &&
            ret->getReturnValue()->getType()->isPointerTy() &&
            isNullable(pointerFact(ret->getReturnValue(), final, nullable)))
          run.returns_null = true;
    }
  if (run.converged && result)
    for (const LoadInst *load : mixed_candidates) {
      const AllocaInst *target =
          scalarCell(load->getPointerOperand(), layout, load->getType());
      if (!target)
        continue;
      struct WitnessState {
        const BasicBlock *block = nullptr;
        const BasicBlock *predecessor = nullptr;
        State state;
        std::set<const BasicBlock *> visited;
      };
      std::deque<WitnessState> witnesses{
          {&function.getEntryBlock(), nullptr, entry, {}}};
      size_t explored = 0;
      bool found = false;
      while (!witnesses.empty() && ++explored <= 1024) {
        WitnessState path = std::move(witnesses.front());
        witnesses.pop_front();
        if (!path.state.reachable || !path.visited.insert(path.block).second)
          continue;
        if (path.block == load->getParent()) {
          State before =
              transfer(*path.block, path.state, false, path.predecessor, load);
          auto cell = before.cells.find(target);
          if (!before.unsupported_guard && cell != before.cells.end() &&
              cell->second.initialization == Uninit) {
            found = true;
            break;
          }
        }
        State after =
            transfer(*path.block, path.state, false, path.predecessor);
        if (!after.reachable)
          continue;
        for (const BasicBlock *successor : successors(path.block)) {
          WitnessState next = path;
          next.block = successor;
          next.predecessor = path.block;
          next.state = after;
          if (const auto *branch =
                  dyn_cast<BranchInst>(path.block->getTerminator()))
            if (branch->isConditional())
              refineCondition(next.state, branch->getCondition(),
                              branch->getSuccessor(0) == successor, nullable);
          // Enum source ranges and default-case exhaustiveness need source
          // type information. Withhold mixed-path diagnostics for such paths.
          if (isa<SwitchInst>(path.block->getTerminator()))
            next.state.unsupported_guard = true;
          if (next.state.reachable)
            witnesses.push_back(std::move(next));
        }
      }
      if (explored > 1024)
        result->convergence_limit_hit = true;
      if (found)
        addFinding(*result, selected, "cpp/not-initialised",
                   "A Guard-consistent CFG path reads this scalar stack cell "
                   "before initialization.",
                   *load, {target});
    }
  if (run.converged && result)
    for (const auto &entry : run.unguarded_dereferences) {
      auto checked = run.guarded_dereferences.find(entry.first);
      if (checked == run.guarded_dereferences.end())
        continue;
      for (const Instruction *site : entry.second)
        addFinding(
            *result, selected, "cpp/inconsistent-nullness-testing",
            "This pointer definition is dereferenced without a nonnull Guard, "
            "while another dereference of the same definition is guarded.",
            *site, {checked->second.front()});
    }
  if (run.converged && result)
    for (const ArgumentUse &use : argument_uses) {
      bool prior = std::any_of(
          guard_uses.begin(), guard_uses.end(), [&](const GuardUse &guard) {
            return guard.source == use.source &&
                   dominators.dominates(guard.branch, use.call);
          });
      if (prior)
        continue;
      for (const GuardUse &guard : guard_uses)
        if (guard.relational_zero && guard.source == use.source &&
            dominators.dominates(use.call, guard.branch)) {
          addFinding(
              *result, selected, "cpp/late-check-of-function-argument",
              "This size or positioning argument is checked against zero after "
              "the call, with no earlier dominating check of the same value.",
              *use.call, {guard.branch});
          break;
        }
    }
  return run;
}

} // namespace

PointerStateFact StateAnalysisResult::stateBefore(const Instruction &instruction,
                                               const Value &pointer) const {
  auto found = states.find(&instruction);
  if (found != states.end())
    for (const auto &fact : found->second)
      if (fact.pointer == &pointer)
        return fact;
  return {&pointer, PointerNullState::Unknown, InitializationState::Unknown};
}

const std::vector<AnalysisRuleDescriptor> &StateAnalysis::catalog() {
  static const std::vector<AnalysisRuleDescriptor> catalog = {
      {"cpp/missing-null-test", "recommendation", "Critical/MissingNullTest.ql",
       "Retained load/store dereferences of null literals or "
       "malloc/calloc/direct nullable function results, constant nonzero "
       "memory transfers, and null literals passed to unconditional "
       "direct-callee dereferences. CFG null guards, SSA copies and exact "
       "scalar pointer cells supported. Unknown aliases and function pointers "
       "excluded; unchecked allocation alone is not a finding."},
      {"cpp/deref-null-result", "recommendation",
       "experimental/Likely Bugs/DerefNullResult.ql",
       "Nullable direct-call result passed to a direct callee parameter whose "
       "retained load/store dominates every reachable normal return. CFG "
       "caller null guards suppress findings. SSA parameters and unmodified "
       "private parameter spills supported; indirect calls and conditional "
       "callee dereferences excluded."},
      {"cpp/uninitialized-local", "warning",
       "Likely Bugs/Memory Management/UninitializedLocal.ql",
       "Definite uninitialized full scalar stack-cell loads on all tracked "
       "predecessors, with strong stores and CFG joins. Arrays/aggregates, "
       "escaped/unknown-clobbered cells, source macro exceptions and "
       "optimized-away undefined reads excluded. Mixed "
       "initialized/uninitialized joins are reported separately by "
       "not-initialised."},
      {"cpp/not-initialised", "error", "Critical/NotInitialised.ql",
       "Local-only conditional subset: full scalar stack-cell reads with "
       "initialized and uninitialized reaching predecessors, reported only "
       "with a bounded Guard-consistent path witness. Stable store/load value "
       "snapshots and signed constant comparisons support repeated/nested "
       "checks. Paths requiring a repeated loop iteration, unsupported "
       "relational arithmetic, switch exhaustiveness, or unknown aliases are "
       "withheld. Definite reads use uninitialized-local; globals excluded."},
      {"cpp/conditionally-uninitialized-variable", "warning",
       "Security/CWE/CWE-457/ConditionallyUninitializedVariable.ql",
       "Defined direct integral-status initializers with bounded acyclic "
       "return/output-parameter summaries. Scalar and exact constant-offset "
       "field writes, transparent initializer wrappers, caller null contexts "
       "and status guards supported. Reports a call only after an initially "
       "uninitialized output is read before a status check. SAL/CSV evidence, "
       "virtual calls, loop summaries and unknown aliases excluded."},
      {"cpp/missing-check-scanf", "warning", "Critical/MissingCheckScanf.ql",
       "Scanf-family numeric and %n output reads with exact local/heap "
       "object provenance and no prior initialization. Return assignment "
       "counts, %* suppression, positional arguments, signed range/equality "
       "guards, stored status, default writes and switch cases supported. "
       "%n needs proven input progress past literals/suppressed conversions; "
       "leading %n and bounded plain-integer constant-sscanf prefixes can "
       "prove unconditional writes. "
       "Boolean-only checks belong to incorrectly-checked-scanf. String/array "
       "writes, unknown aliases, narrowing status casts and source-only call "
       "contexts excluded."},
      {"cpp/improper-check-return-value-scanf", "warning",
       "experimental/Security/CWE/CWE-754/ImproperCheckReturnValueScanf.ql",
       "Ignored scanf-family status followed by assignment/return of an "
       "initially uninitialized numeric scalar output, excluding comparison "
       "uses. Shares conditional-memory effects with missing-check-scanf but "
       "reports the unchecked call. Source macro exclusions and arrays "
       "unavailable; dead unused outputs and checked results excluded."},
      {"cpp/late-check-of-function-argument", "warning",
       "experimental/Security/CWE/CWE-020/LateCheckOfFunctionArgument.ql",
       "Retained standard buffer/positioning calls and memory intrinsics "
       "whose size argument is compared relationally with zero in a later "
       "dominated branch, with no earlier dominating check of the same stable "
       "value. Store/load snapshots prevent matching overwritten variables. "
       "General GVN, source macro exclusions and unresolved calls excluded."},
      {"cpp/inconsistent-nullness-testing", "warning",
       "Critical/InconsistentNullnessTesting.ql",
       "Retained load/store dereferences of one stable pointer definition: "
       "at least one is guarded by a nonnull branch and another is unguarded. "
       "SSA and exact scalar pointer-cell snapshots supported; merged "
       "ambiguous definitions, macro exclusions and implicit API reads "
       "excluded."},
      {"cpp/inconsistent-null-check", "error",
       "Likely Bugs/InconsistentCheckReturnNull.ql",
       "Module-local statistical subset: among direct pointer-returning "
       "calls with retained load/store uses, at least 70% have a null check. "
       "Reports the remaining unchecked calls. Stable stored-pointer origins "
       "and short-circuit checks supported. Macro/assert AST exceptions, "
       "qualified objects and ambiguous merged pointer definitions excluded."}};
  return catalog;
}

StateAnalysisResult
StateAnalysis::analyze(const Module &module,
                    const std::vector<std::string> &rule_ids) const {
  StateAnalysisResult result;
  std::set<std::string> selected(rule_ids.begin(), rule_ids.end());
  NullableFunctions nullable;
  for (const Function &function : module)
    if (ValueFacts::hasLibraryName(function, "malloc", true) ||
        ValueFacts::hasLibraryName(function, "calloc", true))
      nullable.insert(&function);
  ParameterReads reads = summarizeParameterReads(module);
  OutputSummaries output_summaries;
  bool output_limit = false, output_changed = true;
  unsigned output_rounds = 0;
  while (output_changed && output_rounds++ < 8) {
    output_changed = false;
    for (const Function &function : module) {
      if (output_summaries.count(&function))
        continue;
      auto exits =
          summarizeOutputs(function, output_summaries, nullable, output_limit);
      if (!exits.empty() &&
          std::any_of(exits.begin(), exits.end(), [](const OutputExit &exit) {
            return !exit.writes.empty();
          })) {
        output_summaries.emplace(&function, std::move(exits));
        output_changed = true;
      }
    }
  }
  result.convergence_limit_hit |= output_limit || output_changed;
  // Grow null-return summaries across direct wrappers without mixing caller
  // arguments with callee return identities. Bound recursion conservatively.
  bool changed = true;
  unsigned rounds = 0;
  while (changed && rounds++ < 16) {
    changed = false;
    for (const Function &function : module) {
      if (function.isDeclaration() ||
          !function.getReturnType()->isPointerTy() || nullable.count(&function))
        continue;
      FunctionRun run = analyzeFunction(function, nullable, reads,
                                        output_summaries, selected, nullptr);
      result.convergence_limit_hit |= !run.converged;
      if (run.returns_null)
        changed |= nullable.insert(&function).second;
    }
  }
  result.convergence_limit_hit |= changed;
  std::map<const Function *, std::set<const CallBase *>> relevant_calls;
  std::set<const CallBase *> checked_calls;
  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    ++result.analyzed_functions;
    FunctionRun run = analyzeFunction(function, nullable, reads,
                                      output_summaries, selected, &result);
    result.convergence_limit_hit |= !run.converged;
    checked_calls.insert(run.checked_pointer_calls.begin(),
                         run.checked_pointer_calls.end());
    for (const CallBase *call : run.dereferenced_pointer_calls)
      if (const Function *callee = ValueFacts::callee(*call))
        relevant_calls[callee].insert(call);
  }
  for (const auto &group : relevant_calls) {
    size_t checked = 0;
    for (const CallBase *call : group.second)
      checked += checked_calls.count(call);
    unsigned percent =
        static_cast<unsigned>(checked * 100 / group.second.size());
    if (percent < 70)
      continue;
    for (const CallBase *call : group.second)
      if (!checked_calls.count(call))
        addFinding(result, selected, "cpp/inconsistent-null-check",
                   "The result of this call is unchecked for null, while " +
                       std::to_string(percent) +
                       "% of retained dereferenced calls to " +
                       group.first->getName().str() + " check nullness.",
                   *call, {});
  }
  return result;
}

} // namespace pdg
