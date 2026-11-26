/** @file PulseAbductiveDomain.h @brief Abductive abstract domain for Pulse analysis. */
#pragma once

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseMemory.h"
#include "Checker/Pulse/Domain/PulseInvalidation.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Interproc/PulseTransitiveInfo.h"
#include "Checker/Pulse/Topl/PulseTopl.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>

namespace pulse {
class HeapPath;
} // namespace pulse

namespace pulse {

/**
 * Kind of allocator / resource owner attached to an allocated address.
 * Mirrors Infer's Attribute.allocator.
 */
enum class AllocatorKind {
  CMalloc,
  CRealloc,
  CppNew,
  CppNewArray,
  FileHandle,
  Socket,
  Stack,
  Global,
  CustomResource
};

/**
 * Parameterized allocation metadata (Infer's Attribute.Allocated).
 */
struct AllocationInfo {
  AllocatorKind kind{AllocatorKind::CMalloc};
  const llvm::Instruction *location{nullptr};
  Trace trace;
  const llvm::Instruction *unreachable_at{nullptr};

  AllocationInfo() = default;
  AllocationInfo(AllocatorKind k, const llvm::Instruction *loc,
                 Trace t = Trace())
      : kind(k), location(loc), trace(std::move(t)) {}
  AllocationInfo(const AllocationInfo &other)
      : kind(other.kind), location(other.location), trace(other.trace.clone()),
        unreachable_at(other.unreachable_at) {}
  AllocationInfo &operator=(const AllocationInfo &other) {
    if (this != &other) {
      kind = other.kind;
      location = other.location;
      trace = other.trace.clone();
      unreachable_at = other.unreachable_at;
    }
    return *this;
  }
  AllocationInfo(AllocationInfo &&) noexcept = default;
  AllocationInfo &operator=(AllocationInfo &&) noexcept = default;
};

/**
 * Precondition requirement that an address must be valid when accessed.
 * Mirrors Infer's Attribute.MustBeValid (timestamp, trace, reason).
 */
struct MustBeValidInfo {
  uint64_t timestamp{0};
  Trace trace;
  std::optional<InvalidationKind> reason;

  MustBeValidInfo() = default;
  MustBeValidInfo(uint64_t ts, Trace t,
                  std::optional<InvalidationKind> r = std::nullopt)
      : timestamp(ts), trace(std::move(t)), reason(r) {}
  MustBeValidInfo(const MustBeValidInfo &other)
      : timestamp(other.timestamp), trace(other.trace.clone()),
        reason(other.reason) {}
  MustBeValidInfo &operator=(const MustBeValidInfo &other) {
    if (this != &other) {
      timestamp = other.timestamp;
      trace = other.trace.clone();
      reason = other.reason;
    }
    return *this;
  }
  MustBeValidInfo(MustBeValidInfo &&) noexcept = default;
  MustBeValidInfo &operator=(MustBeValidInfo &&) noexcept = default;
};

/**
 * Precondition requirement that an address must be initialized before reading.
 * Mirrors Infer's Attribute.MustBeInitialized (timestamp, trace).
 */
struct MustBeInitializedInfo {
  uint64_t timestamp{0};
  Trace trace;

  MustBeInitializedInfo() = default;
  MustBeInitializedInfo(uint64_t ts, Trace t)
      : timestamp(ts), trace(std::move(t)) {}
  MustBeInitializedInfo(const MustBeInitializedInfo &other)
      : timestamp(other.timestamp), trace(other.trace.clone()) {}
  MustBeInitializedInfo &operator=(const MustBeInitializedInfo &other) {
    if (this != &other) {
      timestamp = other.timestamp;
      trace = other.trace.clone();
    }
    return *this;
  }
  MustBeInitializedInfo(MustBeInitializedInfo &&) noexcept = default;
  MustBeInitializedInfo &operator=(MustBeInitializedInfo &&) noexcept = default;
};

/**
 * Precondition taint sink requirement (Infer's Attribute.MustNotBeTainted).
 */
struct MustNotBeTaintedSink {
  std::string sink_name;
  std::vector<TaintKind> kinds;
  const llvm::Instruction *location{nullptr};
  Trace trace;
  uint64_t timestamp{0};

  MustNotBeTaintedSink() = default;
  MustNotBeTaintedSink(std::string name, std::vector<TaintKind> ks,
                       const llvm::Instruction *loc, Trace t, uint64_t ts)
      : sink_name(std::move(name)), kinds(std::move(ks)), location(loc),
        trace(std::move(t)), timestamp(ts) {}
  MustNotBeTaintedSink(const MustNotBeTaintedSink &other)
      : sink_name(other.sink_name), kinds(other.kinds),
        location(other.location), trace(other.trace.clone()),
        timestamp(other.timestamp) {}
  MustNotBeTaintedSink &operator=(const MustNotBeTaintedSink &other) {
    if (this != &other) {
      sink_name = other.sink_name;
      kinds = other.kinds;
      location = other.location;
      trace = other.trace.clone();
      timestamp = other.timestamp;
    }
    return *this;
  }
  MustNotBeTaintedSink(MustNotBeTaintedSink &&) noexcept = default;
  MustNotBeTaintedSink &operator=(MustNotBeTaintedSink &&) noexcept = default;
};

/**
 * Postcondition record that a memory location was written to
 * (Infer's Attribute.WrittenTo).
 */
struct WrittenToInfo {
  uint64_t timestamp{0};
  const llvm::Instruction *location{nullptr};
  Trace trace;

  WrittenToInfo() = default;
  WrittenToInfo(uint64_t ts, const llvm::Instruction *loc, Trace t = Trace())
      : timestamp(ts), location(loc), trace(std::move(t)) {}
  WrittenToInfo(const WrittenToInfo &other)
      : timestamp(other.timestamp), location(other.location),
        trace(other.trace.clone()) {}
  WrittenToInfo &operator=(const WrittenToInfo &other) {
    if (this != &other) {
      timestamp = other.timestamp;
      location = other.location;
      trace = other.trace.clone();
    }
    return *this;
  }
  WrittenToInfo(WrittenToInfo &&) noexcept = default;
  WrittenToInfo &operator=(WrittenToInfo &&) noexcept = default;
};

/**
 * Postcondition taint propagation summary edge
 * (Infer's Attribute.PropagateTaintFrom).
 */
struct PropagateTaintFromInfo {
  std::vector<AbstractValue> sources;
  std::string reason;
};

/**
 * Postcondition unknown effect marker (Infer's Attribute.UnknownEffect).
 */
struct UnknownEffectInfo {
  std::string callee;
  const llvm::Instruction *location{nullptr};
  ValueHistory history;
};

/**
 * Postcondition taint sanitizer record (Infer's Attribute.TaintSanitized).
 */
struct SanitizerRecord {
  TaintKind kind;
  const llvm::Instruction *location{nullptr};
  uint64_t timestamp{0};
};

/**
 * Structured recursive call edge tracked for mutual recursion cycle detection
 * (Infer's PulseMutualRecursion).
 */
struct RecursiveCallEdge {
  const llvm::Function *callee{nullptr};
  const llvm::Function *caller{nullptr};
  const llvm::Instruction *call_site{nullptr};
  std::vector<AbstractValue> args;
};

/**
 * Outcome of incorporating newly discovered formula equalities/zeros into the
 * heap and attribute domain (Infer's AbductiveDomain.incorporate_new_eqs).
 */
struct IncorporateResult {
  enum class Status { Ok, Unsat, PotentialInvalidAccess };
  Status status{Status::Ok};
  AbstractValue address;
  MustBeValidInfo must_be_valid;

  bool isOk() const { return status == Status::Ok; }
  bool isUnsat() const { return status == Status::Unsat; }
  bool isPotentialInvalidAccess() const {
    return status == Status::PotentialInvalidAccess;
  }

  static IncorporateResult ok() { return IncorporateResult{}; }
  static IncorporateResult unsat() {
    IncorporateResult r;
    r.status = Status::Unsat;
    return r;
  }
  static IncorporateResult potentialInvalidAccess(AbstractValue a,
                                                  MustBeValidInfo mbv) {
    IncorporateResult r;
    r.status = Status::PotentialInvalidAccess;
    r.address = a;
    r.must_be_valid = std::move(mbv);
    return r;
  }
};

/**
 * AbductiveDomain: the core bi-abductive abstract domain.
 * Maintains:
 * - Post state: current abstract stack, heap, attributes, parameterized attrs
 * - Pre state: inferred footprint (stack, registered pre-heap, attributes,
 *   MustBeValid, MustBeInitialized, MustNotBeTainted)
 * - Path condition: PulseFormula with branch conditions and NewEqs
 */
class AbductiveDomain {
private:
  Stack post_stack_;
  Heap post_heap_;
  AddressAttributes post_attrs_;

  Stack pre_stack_;
  Heap pre_heap_;
  AddressAttributes pre_attrs_;

  // Parameterized precondition attributes (Infer PulseAttribute)
  std::map<AbstractValue, MustBeValidInfo> pre_must_be_valid_;
  std::map<AbstractValue, MustBeInitializedInfo> pre_must_be_initialized_;
  std::map<AbstractValue, std::vector<MustNotBeTaintedSink>>
      pre_must_not_be_tainted_;

  // Parameterized postcondition attributes (Infer PulseAttribute)
  std::map<AbstractValue, WrittenToInfo> post_written_to_;
  std::map<AbstractValue, AllocationInfo> post_allocation_info_;
  std::map<AbstractValue, std::vector<PropagateTaintFromInfo>>
      post_propagate_taint_from_;
  std::map<AbstractValue, UnknownEffectInfo> post_unknown_effect_;
  std::map<AbstractValue, std::vector<SanitizerRecord>> post_taint_sanitized_;

  // Dynamic types & lightweight decompiler expressions
  std::map<AbstractValue, std::string> dynamic_types_;
  std::map<AbstractValue, std::string> decompiler_;

  // Taint tracking & TOPL history
  TaintDomain taint_domain_;
  ToplHistory topl_history_;

  // Path condition: formula tracking equalities and constraints
  std::unique_ptr<PulseFormula> path_formula_;

  // Invalidation kind + location per invalid address
  std::map<AbstractValue,
           std::pair<InvalidationKind, const llvm::Instruction *>>
      invalidation_info_;

  // Allocation sizes (in bytes) for base addresses when known (constant or
  // symbolic AbstractValue for dynamic-size alloca/malloc).
  std::map<AbstractValue, uint64_t> allocation_sizes_;
  std::map<AbstractValue, AbstractValue> symbolic_allocation_sizes_;

  // Objects managed by strong reference counting (std::shared_ptr targets) for
  // RetainCycle detection.
  std::set<AbstractValue> shared_ptr_targets_;

  // Stable allocation identity for base and derived pointer values
  std::map<AbstractValue, AbstractValue> allocation_roots_;

  // Skipped calls (for unknown functions)
  std::set<std::string> skipped_calls_;

  // Interprocedural transitive info
  TransitiveInfo transitive_info_;

  // Did we generate at least one unknown abstract value on this path?
  bool unknown_values_ = false;

  // Values requiring dynamic type specialization
  std::set<AbstractValue> need_dynamic_type_specialization_;

  // Mutually recursive calls observed along this path
  std::set<std::string> recursive_calls_;
  std::vector<RecursiveCallEdge> recursive_call_edges_;

  // Per-disjunct nested loop header stack & iteration path stamps
  // (Infer PulseLoopHeaderInfo.ml).
  struct LoopHeaderFrame {
    const llvm::BasicBlock *header{nullptr};
    unsigned visits{0};
    size_t last_stamp{0};
  };
  std::vector<LoopHeaderFrame> loop_header_stack_;
  std::map<const llvm::BasicBlock *, unsigned> loop_header_info_;
  std::map<const llvm::BasicBlock *, std::vector<size_t>> loop_path_stamps_;

  // Monotonic per-domain operation timestamp for MustBeValid/WrittenTo ordering
  uint64_t timestamp_counter_{1};

  // Loop invariant under inference: optional (header BB, entry state)
  struct LoopInvariantUnderInference {
    const llvm::BasicBlock *header;
    std::unique_ptr<AbductiveDomain> entry_astate;

    LoopInvariantUnderInference(const llvm::BasicBlock *h,
                                AbductiveDomain astate)
        : header(h),
          entry_astate(std::make_unique<AbductiveDomain>(std::move(astate))) {}
  };
  std::unique_ptr<LoopInvariantUnderInference> loop_invariant_under_inference_;

public:
  ToplHistory &getToplHistory() { return topl_history_; }
  const ToplHistory &getToplHistory() const { return topl_history_; }

  // Stack operations
  Stack &getPostStack() { return post_stack_; }
  Stack &getPreStack() { return pre_stack_; }
  const Stack &getPostStack() const { return post_stack_; }
  const Stack &getPreStack() const { return pre_stack_; }

  // Heap operations
  Heap &getPostHeap() { return post_heap_; }
  Heap &getPreHeap() { return pre_heap_; }
  const Heap &getPostHeap() const { return post_heap_; }
  const Heap &getPreHeap() const { return pre_heap_; }

  // Attribute operations
  AddressAttributes &getPostAttrs() { return post_attrs_; }
  AddressAttributes &getPreAttrs() { return pre_attrs_; }
  const AddressAttributes &getPostAttrs() const { return post_attrs_; }
  const AddressAttributes &getPreAttrs() const { return pre_attrs_; }

  // Taint operations
  TaintDomain &getTaintDomain() { return taint_domain_; }
  const TaintDomain &getTaintDomain() const { return taint_domain_; }

  // Path condition
  PulseFormula &getPathFormula();
  const PulseFormula &getPathFormula() const;
  void setPathFormula(std::unique_ptr<PulseFormula> formula);

  // Monotonic operation timestamps
  uint64_t nextTimestamp() { return timestamp_counter_++; }
  uint64_t getTimestampCounter() const { return timestamp_counter_; }
  void setTimestampCounter(uint64_t ts) { timestamp_counter_ = ts; }

  // Convenience methods for path conditions
  void addNonNull(AbstractValue addr);
  bool isNonNull(AbstractValue addr) const;
  void addEquality(AbstractValue v1, AbstractValue v2);
  AbstractValue getCanonical(AbstractValue v) const;

  // Precondition footprint registration & queries (Infer's is_in_pre /
  // BaseMemory.register_address)
  void registerPreAddress(AbstractValue addr);
  bool isInPre(AbstractValue addr) const;
  bool isAllocated(AbstractValue addr) const;
  bool preHeapHasAssumptions() const;

  // Conditional bi-abduction (gated on isInPre(addr))
  void abduceToPre(AbstractValue addr, const Access &access, Address target);
  void abduceAttrToPre(AbstractValue addr, Attribute attr);
  void abduceMustBeValid(AbstractValue addr, Trace trace,
                         std::optional<InvalidationKind> reason = std::nullopt);
  void abduceMustBeInitialized(AbstractValue addr, Trace trace);
  void abduceMustNotBeTainted(AbstractValue addr, MustNotBeTaintedSink sink);

  // Parameterized precondition attribute accessors
  const std::map<AbstractValue, MustBeValidInfo> &getPreMustBeValid() const {
    return pre_must_be_valid_;
  }
  std::map<AbstractValue, MustBeValidInfo> &getPreMustBeValid() {
    return pre_must_be_valid_;
  }
  const std::map<AbstractValue, MustBeInitializedInfo> &
  getPreMustBeInitialized() const {
    return pre_must_be_initialized_;
  }
  std::map<AbstractValue, MustBeInitializedInfo> &getPreMustBeInitialized() {
    return pre_must_be_initialized_;
  }
  const std::map<AbstractValue, std::vector<MustNotBeTaintedSink>> &
  getPreMustNotBeTainted() const {
    return pre_must_not_be_tainted_;
  }
  std::map<AbstractValue, std::vector<MustNotBeTaintedSink>> &
  getPreMustNotBeTainted() {
    return pre_must_not_be_tainted_;
  }

  // Parameterized postcondition attribute accessors/mutators
  void markWrittenTo(AbstractValue addr, const llvm::Instruction *loc,
                     Trace trace = Trace());
  bool hasWrittenTo(AbstractValue addr) const;
  std::optional<WrittenToInfo> getWrittenTo(AbstractValue addr) const;
  const std::map<AbstractValue, WrittenToInfo> &getPostWrittenTo() const {
    return post_written_to_;
  }
  std::map<AbstractValue, WrittenToInfo> &getPostWrittenTo() {
    return post_written_to_;
  }

  void setAllocationInfo(AbstractValue addr, AllocationInfo info);
  std::optional<AllocationInfo> getAllocationInfo(AbstractValue addr) const;
  void removeAllocationInfo(AbstractValue addr);
  const std::map<AbstractValue, AllocationInfo> &getPostAllocationInfo() const {
    return post_allocation_info_;
  }
  std::map<AbstractValue, AllocationInfo> &getPostAllocationInfo() {
    return post_allocation_info_;
  }

  void addPropagateTaintFrom(AbstractValue dest, PropagateTaintFromInfo info);
  const std::map<AbstractValue, std::vector<PropagateTaintFromInfo>> &
  getPostPropagateTaintFrom() const {
    return post_propagate_taint_from_;
  }
  std::map<AbstractValue, std::vector<PropagateTaintFromInfo>> &
  getPostPropagateTaintFrom() {
    return post_propagate_taint_from_;
  }

  void setUnknownEffect(AbstractValue addr, UnknownEffectInfo info);
  std::optional<UnknownEffectInfo> getUnknownEffect(AbstractValue addr) const;
  const std::map<AbstractValue, UnknownEffectInfo> &
  getPostUnknownEffect() const {
    return post_unknown_effect_;
  }
  std::map<AbstractValue, UnknownEffectInfo> &getPostUnknownEffect() {
    return post_unknown_effect_;
  }

  void addTaintSanitized(AbstractValue addr, SanitizerRecord rec);
  const std::map<AbstractValue, std::vector<SanitizerRecord>> &
  getPostTaintSanitized() const {
    return post_taint_sanitized_;
  }
  std::map<AbstractValue, std::vector<SanitizerRecord>> &
  getPostTaintSanitized() {
    return post_taint_sanitized_;
  }

  // Dynamic types & decompiler metadata
  void setDynamicType(AbstractValue addr, const std::string &type_name);
  std::optional<std::string> getDynamicType(AbstractValue addr) const;
  const std::map<AbstractValue, std::string> &getDynamicTypes() const {
    return dynamic_types_;
  }
  std::map<AbstractValue, std::string> &getDynamicTypes() {
    return dynamic_types_;
  }

  void setDecompilerName(AbstractValue addr, const std::string &name);
  std::optional<std::string> getDecompilerName(AbstractValue addr) const;
  const std::map<AbstractValue, std::string> &getDecompilerMap() const {
    return decompiler_;
  }
  std::map<AbstractValue, std::string> &getDecompilerMap() {
    return decompiler_;
  }

  // Invalidation info (for UseAfterFree diagnostics)
  void setInvalidationInfo(AbstractValue addr, InvalidationKind kind,
                           const llvm::Instruction *loc);
  std::optional<std::pair<InvalidationKind, const llvm::Instruction *>>
  getInvalidationInfo(AbstractValue addr) const;

  // Allocation size tracking
  void setAllocationSize(AbstractValue addr, uint64_t size_bytes) {
    AbstractValue canon = getCanonical(addr);
    if (size_bytes == 0) {
      allocation_sizes_.erase(canon);
      allocation_sizes_.erase(addr);
    } else {
      allocation_sizes_[canon] = size_bytes;
    }
  }
  std::optional<uint64_t> getAllocationSize(AbstractValue addr) const {
    AbstractValue canon = getCanonical(addr);
    auto it = allocation_sizes_.find(canon);
    if (it != allocation_sizes_.end()) {
      return it->second;
    }
    it = allocation_sizes_.find(addr);
    if (it != allocation_sizes_.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  const std::map<AbstractValue, uint64_t> &getAllocationSizes() const {
    return allocation_sizes_;
  }
  std::map<AbstractValue, uint64_t> &getAllocationSizes() {
    return allocation_sizes_;
  }

  void setSymbolicAllocationSize(AbstractValue addr, AbstractValue size_av) {
    symbolic_allocation_sizes_[getCanonical(addr)] = getCanonical(size_av);
  }
  std::optional<AbstractValue>
  getSymbolicAllocationSize(AbstractValue addr) const {
    AbstractValue c = getCanonical(addr);
    auto it = symbolic_allocation_sizes_.find(c);
    if (it != symbolic_allocation_sizes_.end()) {
      return getCanonical(it->second);
    }
    return std::nullopt;
  }
  const std::map<AbstractValue, AbstractValue> &
  getSymbolicAllocationSizes() const {
    return symbolic_allocation_sizes_;
  }
  std::map<AbstractValue, AbstractValue> &getSymbolicAllocationSizes() {
    return symbolic_allocation_sizes_;
  }

  void addSharedPtrTarget(AbstractValue target) {
    shared_ptr_targets_.insert(getCanonical(target));
  }
  const std::set<AbstractValue> &getSharedPtrTargets() const {
    return shared_ptr_targets_;
  }
  std::set<AbstractValue> &getSharedPtrTargets() { return shared_ptr_targets_; }

  void setAllocationRoot(AbstractValue addr, AbstractValue root) {
    allocation_roots_[getCanonical(addr)] = getCanonical(root);
  }
  AbstractValue getAllocationRoot(AbstractValue addr) const {
    AbstractValue canon_addr = getCanonical(addr);
    auto it = allocation_roots_.find(canon_addr);
    if (it == allocation_roots_.end()) {
      it = allocation_roots_.find(addr);
    }
    return it == allocation_roots_.end() ? canon_addr
                                         : getCanonical(it->second);
  }
  const std::map<AbstractValue, AbstractValue> &getAllocationRoots() const {
    return allocation_roots_;
  }
  std::map<AbstractValue, AbstractValue> &getAllocationRoots() {
    return allocation_roots_;
  }

  // Skipped calls
  void addSkippedCall(const std::string &name) { skipped_calls_.insert(name); }
  const std::set<std::string> &getSkippedCalls() const {
    return skipped_calls_;
  }
  bool hasSkippedCall(const std::string &name) const {
    return skipped_calls_.count(name) > 0;
  }

  // TransitiveInfo operations
  TransitiveInfo &getTransitiveInfo() { return transitive_info_; }
  const TransitiveInfo &getTransitiveInfo() const { return transitive_info_; }
  void setTransitiveInfo(const TransitiveInfo &info) {
    transitive_info_ = info;
  }

  // Unknown values flag
  bool hasUnknownValues() const { return unknown_values_; }
  void setUnknownValues(bool val) { unknown_values_ = val; }
  void declareUnknownValues() { unknown_values_ = true; }

  // Dynamic type specialization
  const std::set<AbstractValue> &getNeedDynamicTypeSpecialization() const {
    return need_dynamic_type_specialization_;
  }
  void addNeedDynamicTypeSpecialization(AbstractValue av) {
    need_dynamic_type_specialization_.insert(getCanonical(av));
  }
  void clearNeedDynamicTypeSpecialization() {
    need_dynamic_type_specialization_.clear();
  }

  // Recursive calls
  const std::set<std::string> &getRecursiveCalls() const {
    return recursive_calls_;
  }
  void addRecursiveCall(const std::string &name) {
    recursive_calls_.insert(name);
  }
  void addRecursiveCalls(const std::set<std::string> &calls) {
    recursive_calls_.insert(calls.begin(), calls.end());
  }
  const std::vector<RecursiveCallEdge> &getRecursiveCallEdges() const {
    return recursive_call_edges_;
  }
  std::vector<RecursiveCallEdge> &getRecursiveCallEdges() {
    return recursive_call_edges_;
  }
  void addRecursiveCallEdge(RecursiveCallEdge edge) {
    if (recursive_call_edges_.size() < 16) {
      recursive_call_edges_.push_back(std::move(edge));
    }
  }

  // Loop header info & per-disjunct path stamps
  const std::map<const llvm::BasicBlock *, unsigned> &
  getLoopHeaderInfo() const {
    return loop_header_info_;
  }
  std::map<const llvm::BasicBlock *, unsigned> &getLoopHeaderInfo() {
    return loop_header_info_;
  }
  void initLoopHeaderInfo(const llvm::BasicBlock *header) {
    loop_header_info_[header] = 0;
  }
  void removeLoopHeaderInfo(const llvm::BasicBlock *header) {
    loop_header_info_.erase(header);
    loop_path_stamps_.erase(header);
  }
  void pushLoopHeaderInfo(const llvm::BasicBlock *header, unsigned timestamp) {
    loop_header_info_[header] = timestamp;
  }
  const std::map<const llvm::BasicBlock *, std::vector<size_t>> &
  getLoopPathStamps() const {
    return loop_path_stamps_;
  }
  std::map<const llvm::BasicBlock *, std::vector<size_t>> &getLoopPathStamps() {
    return loop_path_stamps_;
  }
  bool hasLoopPathStamp(const llvm::BasicBlock *header, size_t stamp) const {
    auto it = loop_path_stamps_.find(header);
    if (it == loop_path_stamps_.end()) {
      return false;
    }
    for (size_t s : it->second) {
      if (s == stamp) {
        return true;
      }
    }
    return false;
  }
  void recordLoopPathStamp(const llvm::BasicBlock *header, size_t stamp) {
    loop_path_stamps_[header].push_back(stamp);
    loop_header_info_[header] =
        static_cast<unsigned>(loop_path_stamps_[header].size());
    for (auto &frame : loop_header_stack_) {
      if (frame.header == header) {
        frame.visits++;
        frame.last_stamp = stamp;
        return;
      }
    }
    loop_header_stack_.push_back(LoopHeaderFrame{header, 1u, stamp});
  }
  unsigned getDisjunctLoopVisits(const llvm::BasicBlock *header) const {
    for (const auto &frame : loop_header_stack_) {
      if (frame.header == header)
        return frame.visits;
    }
    return 0u;
  }
  const std::vector<LoopHeaderFrame> &getLoopHeaderStack() const {
    return loop_header_stack_;
  }
  void popLoopHeader(const llvm::BasicBlock *header) {
    for (auto it = loop_header_stack_.begin(); it != loop_header_stack_.end();
         ++it) {
      if (it->header == header) {
        loop_header_stack_.erase(it, loop_header_stack_.end());
        return;
      }
    }
  }

  // Loop invariant under inference
  bool isLoopInvariantUnderInference(const llvm::BasicBlock *header) const {
    return loop_invariant_under_inference_ &&
           loop_invariant_under_inference_->header == header;
  }
  bool isSomeLoopInvariantUnderInference() const {
    return loop_invariant_under_inference_ != nullptr;
  }
  void startLoopInvariantInference(const llvm::BasicBlock *header,
                                   AbductiveDomain entry_astate) {
    loop_invariant_under_inference_ =
        std::make_unique<LoopInvariantUnderInference>(header,
                                                      std::move(entry_astate));
  }
  void clearLoopInvariantInference() {
    loop_invariant_under_inference_.reset();
  }

  // Constructors and assignment operators
  AbductiveDomain() : path_formula_(nullptr) {}
  ~AbductiveDomain();

  // Delete copy operations (use clone() instead)
  AbductiveDomain(const AbductiveDomain &) = delete;
  AbductiveDomain &operator=(const AbductiveDomain &) = delete;

  // Move operations
  AbductiveDomain(AbductiveDomain &&) = default;
  AbductiveDomain &operator=(AbductiveDomain &&) = default;

  // Clone domain
  AbductiveDomain clone() const;

  // Normalize internal maps (stack/heap/attrs) w.r.t. current path formula.
  void canonicalize();

  // Incorporate newly discovered formula equalities / zeros into heaps and
  // attributes (Infer's AbductiveDomain.incorporate_new_eqs).
  IncorporateResult incorporateNewEqs(const NewEqs &new_eqs);
  IncorporateResult incorporatePendingNewEqs();

  // Graph-isomorphism partial order (Infer's GraphComparison.is_isograph).
  static bool leq(const AbductiveDomain &lhs, const AbductiveDomain &rhs);

  // Merge two domains (for joining exit states)
  static std::optional<AbductiveDomain> merge(const AbductiveDomain &d1,
                                               const AbductiveDomain &d2);
};

} // namespace pulse
