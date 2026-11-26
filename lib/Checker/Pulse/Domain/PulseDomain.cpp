#include "Checker/Pulse/Domain/PulseDomain.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Interproc/PulseTransitiveInfo.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <optional>
#include <queue>

namespace pulse {

//===----------------------------------------------------------------------===//
// Stack Implementation
//===----------------------------------------------------------------------===//

void Stack::add(const llvm::Value *var, Address addr) { stack_[var] = addr; }

Address *Stack::find(const llvm::Value *var) {
  auto it = stack_.find(var);
  return (it != stack_.end()) ? &it->second : nullptr;
}

const Address *Stack::find(const llvm::Value *var) const {
  auto it = stack_.find(var);
  return (it != stack_.end()) ? &it->second : nullptr;
}

void Stack::remove(const llvm::Value *var) { stack_.erase(var); }

void Stack::clear() { stack_.clear(); }

//===----------------------------------------------------------------------===//
// Heap Implementation
//===----------------------------------------------------------------------===//

void Heap::registerAddress(AbstractValue addr) { edges_.try_emplace(addr); }

bool Heap::hasAddress(AbstractValue addr) const {
  return edges_.find(addr) != edges_.end();
}

void Heap::addEdge(AbstractValue from, Access access, Address to) {
  edges_[from][access] = to;
}

Address *Heap::findEdge(AbstractValue from, Access access) {
  auto it = edges_.find(from);
  if (it == edges_.end())
    return nullptr;
  auto acc_it = it->second.find(access);
  return (acc_it != it->second.end()) ? &acc_it->second : nullptr;
}

const Address *Heap::findEdge(AbstractValue from, Access access) const {
  auto it = edges_.find(from);
  if (it == edges_.end())
    return nullptr;
  auto acc_it = it->second.find(access);
  return (acc_it != it->second.end()) ? &acc_it->second : nullptr;
}

void Heap::removeEdge(AbstractValue from, const Access &access) {
  auto it = edges_.find(from);
  if (it != edges_.end()) {
    it->second.erase(access);
  }
}

void Heap::removeEdges(AbstractValue addr) { edges_.erase(addr); }

//===----------------------------------------------------------------------===//
// AddressAttributes Implementation
//===----------------------------------------------------------------------===//

void AddressAttributes::add(AbstractValue addr, Attribute attr) {
  attrs_[addr].insert(attr);
}

void AddressAttributes::remove(AbstractValue addr, Attribute attr) {
  auto it = attrs_.find(addr);
  if (it != attrs_.end()) {
    it->second.erase(attr);
    if (it->second.empty()) {
      attrs_.erase(it);
    }
  }
}

bool AddressAttributes::has(AbstractValue addr, Attribute attr) const {
  auto it = attrs_.find(addr);
  return (it != attrs_.end()) && (it->second.count(attr) > 0);
}

AttributeSet AddressAttributes::get(AbstractValue addr) const {
  auto it = attrs_.find(addr);
  return (it != attrs_.end()) ? it->second : AttributeSet();
}

void AddressAttributes::clear(AbstractValue addr) { attrs_.erase(addr); }

//===----------------------------------------------------------------------===//
// AbductiveDomain Implementation
//===----------------------------------------------------------------------===//

AbductiveDomain::~AbductiveDomain() = default;

PulseFormula &AbductiveDomain::getPathFormula() {
  if (!path_formula_) {
    path_formula_ = std::make_unique<PulseFormula>();
  }
  return *path_formula_;
}

const PulseFormula &AbductiveDomain::getPathFormula() const {
  if (!path_formula_) {
    const_cast<AbductiveDomain *>(this)->path_formula_ =
        std::make_unique<PulseFormula>();
  }
  return *path_formula_;
}

void AbductiveDomain::setPathFormula(std::unique_ptr<PulseFormula> formula) {
  path_formula_ = std::move(formula);
}

void AbductiveDomain::addNonNull(AbstractValue addr) {
  (void)getPathFormula().addNonNull(addr);
}

bool AbductiveDomain::isNonNull(AbstractValue addr) const {
  return getPathFormula().isNonNull(addr);
}

void AbductiveDomain::addEquality(AbstractValue v1, AbstractValue v2) {
  (void)getPathFormula().addEquality(v1, v2);
}

AbstractValue AbductiveDomain::getCanonical(AbstractValue v) const {
  return getPathFormula().getCanonical(v);
}

void AbductiveDomain::registerPreAddress(AbstractValue addr) {
  AbstractValue canon = getCanonical(addr);
  pre_heap_.registerAddress(canon);
}

bool AbductiveDomain::isInPre(AbstractValue addr) const {
  AbstractValue canon = getCanonical(addr);
  if (pre_heap_.hasAddress(canon) || pre_heap_.hasAddress(addr)) {
    return true;
  }
  for (const auto &kv : pre_stack_.getMap()) {
    if (getCanonical(kv.second.addr) == canon) {
      return true;
    }
  }
  return false;
}

bool AbductiveDomain::isAllocated(AbstractValue addr) const {
  AbstractValue canon = getCanonical(addr);
  if (post_attrs_.has(canon, Attribute::Allocated) ||
      post_attrs_.has(canon, Attribute::Stack) ||
      post_attrs_.has(canon, Attribute::Global)) {
    return true;
  }
  return post_allocation_info_.find(canon) != post_allocation_info_.end();
}

bool AbductiveDomain::preHeapHasAssumptions() const {
  // Infer PulseAbductiveDomain.ml:2367-2389:
  // True if distinct pre-stack entries alias to the same address, or if two
  // distinct edges in pre_heap_ point to the same canonical target address
  // (i.e. aliasing/sharing assumption in the precondition).
  std::set<AbstractValue> seen_targets;
  for (const auto &kv : pre_stack_.getMap()) {
    AbstractValue canon = getCanonical(kv.second.addr);
    if (!seen_targets.insert(canon).second) {
      return true;
    }
  }
  for (const auto &kv : pre_heap_.getEdges()) {
    for (const auto &edge_kv : kv.second) {
      AbstractValue target_canon = getCanonical(edge_kv.second.addr);
      if (!seen_targets.insert(target_canon).second) {
        return true;
      }
    }
  }
  return false;
}

void AbductiveDomain::abduceToPre(AbstractValue addr, const Access &access,
                                  Address target) {
  AbstractValue canon_addr = getCanonical(addr);
  if (!isInPre(canon_addr)) {
    return;
  }
  Access canon_access = access;
  if (canon_access.kind == AccessKind::ArrayIndex) {
    canon_access.index = getCanonical(canon_access.index);
  }
  Address canon_target(getCanonical(target.addr));
  canon_target.history = target.history;
  pre_heap_.registerAddress(canon_addr);
  if (!pre_heap_.findEdge(canon_addr, canon_access)) {
    pre_heap_.addEdge(canon_addr, canon_access, canon_target);
  }
  pre_heap_.registerAddress(canon_target.addr);
}

void AbductiveDomain::abduceAttrToPre(AbstractValue addr, Attribute attr) {
  AbstractValue canon_addr = getCanonical(addr);
  if (!isInPre(canon_addr)) {
    return;
  }
  pre_heap_.registerAddress(canon_addr);
  if (!pre_attrs_.has(canon_addr, attr)) {
    pre_attrs_.add(canon_addr, attr);
  }
}

void AbductiveDomain::abduceMustBeValid(
    AbstractValue addr, Trace trace, std::optional<InvalidationKind> reason) {
  AbstractValue canon_addr = getCanonical(addr);
  if (!isInPre(canon_addr)) {
    return;
  }
  // Do not require caller validity for locally allocated storage.
  if (isAllocated(canon_addr)) {
    return;
  }
  pre_heap_.registerAddress(canon_addr);
  if (pre_must_be_valid_.find(canon_addr) == pre_must_be_valid_.end()) {
    pre_must_be_valid_.emplace(
        canon_addr, MustBeValidInfo(nextTimestamp(), std::move(trace), reason));
  }
}

void AbductiveDomain::abduceMustBeInitialized(AbstractValue addr, Trace trace) {
  AbstractValue canon_addr = getCanonical(addr);
  if (!isInPre(canon_addr)) {
    return;
  }
  if (hasWrittenTo(canon_addr) || isAllocated(canon_addr)) {
    return;
  }
  pre_heap_.registerAddress(canon_addr);
  if (pre_must_be_initialized_.find(canon_addr) ==
      pre_must_be_initialized_.end()) {
    pre_must_be_initialized_.emplace(
        canon_addr, MustBeInitializedInfo(nextTimestamp(), std::move(trace)));
  }
}

void AbductiveDomain::abduceMustNotBeTainted(AbstractValue addr,
                                             MustNotBeTaintedSink sink) {
  AbstractValue canon_addr = getCanonical(addr);
  if (!isInPre(canon_addr)) {
    return;
  }
  pre_heap_.registerAddress(canon_addr);
  if (sink.timestamp == 0) {
    sink.timestamp = nextTimestamp();
  }
  pre_must_not_be_tainted_[canon_addr].push_back(std::move(sink));
}

void AbductiveDomain::markWrittenTo(AbstractValue addr,
                                    const llvm::Instruction *loc, Trace trace) {
  AbstractValue canon = getCanonical(addr);
  post_written_to_[canon] = WrittenToInfo(nextTimestamp(), loc, std::move(trace));
}

bool AbductiveDomain::hasWrittenTo(AbstractValue addr) const {
  AbstractValue canon = getCanonical(addr);
  return post_written_to_.find(canon) != post_written_to_.end() ||
         post_written_to_.find(addr) != post_written_to_.end();
}

std::optional<WrittenToInfo>
AbductiveDomain::getWrittenTo(AbstractValue addr) const {
  AbstractValue canon = getCanonical(addr);
  auto it = post_written_to_.find(canon);
  if (it != post_written_to_.end()) {
    return it->second;
  }
  it = post_written_to_.find(addr);
  if (it != post_written_to_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void AbductiveDomain::setAllocationInfo(AbstractValue addr,
                                        AllocationInfo info) {
  AbstractValue canon = getCanonical(addr);
  post_allocation_info_[canon] = std::move(info);
}

std::optional<AllocationInfo>
AbductiveDomain::getAllocationInfo(AbstractValue addr) const {
  AbstractValue canon = getCanonical(addr);
  auto it = post_allocation_info_.find(canon);
  if (it != post_allocation_info_.end()) {
    return it->second;
  }
  it = post_allocation_info_.find(addr);
  if (it != post_allocation_info_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void AbductiveDomain::removeAllocationInfo(AbstractValue addr) {
  AbstractValue canon = getCanonical(addr);
  post_allocation_info_.erase(canon);
  post_allocation_info_.erase(addr);
}

void AbductiveDomain::addPropagateTaintFrom(AbstractValue dest,
                                            PropagateTaintFromInfo info) {
  AbstractValue canon = getCanonical(dest);
  for (AbstractValue &src : info.sources) {
    src = getCanonical(src);
  }
  post_propagate_taint_from_[canon].push_back(std::move(info));
}

void AbductiveDomain::setUnknownEffect(AbstractValue addr,
                                       UnknownEffectInfo info) {
  AbstractValue canon = getCanonical(addr);
  post_unknown_effect_[canon] = std::move(info);
}

std::optional<UnknownEffectInfo>
AbductiveDomain::getUnknownEffect(AbstractValue addr) const {
  AbstractValue canon = getCanonical(addr);
  auto it = post_unknown_effect_.find(canon);
  if (it != post_unknown_effect_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void AbductiveDomain::addTaintSanitized(AbstractValue addr,
                                        SanitizerRecord rec) {
  AbstractValue canon = getCanonical(addr);
  if (rec.timestamp == 0) {
    rec.timestamp = nextTimestamp();
  }
  post_taint_sanitized_[canon].push_back(std::move(rec));
}

void AbductiveDomain::setDynamicType(AbstractValue addr,
                                     const std::string &type_name) {
  if (!type_name.empty()) {
    AbstractValue canon = getCanonical(addr);
    dynamic_types_[canon] = type_name;
    (void)getPathFormula().addExactType(canon, type_name);
  }
}

std::optional<std::string>
AbductiveDomain::getDynamicType(AbstractValue addr) const {
  AbstractValue canon = getCanonical(addr);
  auto it = dynamic_types_.find(canon);
  if (it != dynamic_types_.end()) {
    return it->second;
  }
  it = dynamic_types_.find(addr);
  if (it != dynamic_types_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void AbductiveDomain::setDecompilerName(AbstractValue addr,
                                        const std::string &name) {
  if (!name.empty()) {
    decompiler_[getCanonical(addr)] = name;
  }
}

std::optional<std::string>
AbductiveDomain::getDecompilerName(AbstractValue addr) const {
  AbstractValue canon = getCanonical(addr);
  auto it = decompiler_.find(canon);
  if (it != decompiler_.end()) {
    return it->second;
  }
  it = decompiler_.find(addr);
  if (it != decompiler_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void AbductiveDomain::setInvalidationInfo(AbstractValue addr,
                                          InvalidationKind kind,
                                          const llvm::Instruction *loc) {
  invalidation_info_[getCanonical(addr)] = {kind, loc};
}

std::optional<std::pair<InvalidationKind, const llvm::Instruction *>>
AbductiveDomain::getInvalidationInfo(AbstractValue addr) const {
  AbstractValue canon = getCanonical(addr);
  auto it = invalidation_info_.find(canon);
  if (it != invalidation_info_.end())
    return it->second;
  it = invalidation_info_.find(addr);
  if (it != invalidation_info_.end())
    return it->second;
  return std::nullopt;
}

AbductiveDomain AbductiveDomain::clone() const {
  AbductiveDomain cloned;

  cloned.post_stack_ = post_stack_;
  cloned.pre_stack_ = pre_stack_;
  cloned.post_heap_ = post_heap_;
  cloned.pre_heap_ = pre_heap_;
  cloned.post_attrs_ = post_attrs_;
  cloned.pre_attrs_ = pre_attrs_;

  cloned.pre_must_be_valid_ = pre_must_be_valid_;
  cloned.pre_must_be_initialized_ = pre_must_be_initialized_;
  cloned.pre_must_not_be_tainted_ = pre_must_not_be_tainted_;
  cloned.post_written_to_ = post_written_to_;
  cloned.post_allocation_info_ = post_allocation_info_;
  cloned.post_propagate_taint_from_ = post_propagate_taint_from_;
  cloned.post_unknown_effect_ = post_unknown_effect_;
  cloned.post_taint_sanitized_ = post_taint_sanitized_;
  cloned.dynamic_types_ = dynamic_types_;
  cloned.decompiler_ = decompiler_;

  if (path_formula_) {
    cloned.path_formula_ =
        std::make_unique<PulseFormula>(path_formula_->clone());
  }

  cloned.invalidation_info_ = invalidation_info_;
  cloned.allocation_sizes_ = allocation_sizes_;
  cloned.symbolic_allocation_sizes_ = symbolic_allocation_sizes_;
  cloned.shared_ptr_targets_ = shared_ptr_targets_;
  cloned.allocation_roots_ = allocation_roots_;
  cloned.taint_domain_.join(taint_domain_);
  cloned.topl_history_ = topl_history_;

  cloned.skipped_calls_ = skipped_calls_;
  cloned.transitive_info_ = transitive_info_.clone();
  cloned.unknown_values_ = unknown_values_;
  cloned.need_dynamic_type_specialization_ = need_dynamic_type_specialization_;
  cloned.recursive_calls_ = recursive_calls_;
  cloned.recursive_call_edges_ = recursive_call_edges_;
  cloned.loop_header_stack_ = loop_header_stack_;
  cloned.loop_header_info_ = loop_header_info_;
  cloned.loop_path_stamps_ = loop_path_stamps_;
  cloned.timestamp_counter_ = timestamp_counter_;

  if (loop_invariant_under_inference_) {
    cloned.loop_invariant_under_inference_ =
        std::make_unique<LoopInvariantUnderInference>(
            loop_invariant_under_inference_->header,
            loop_invariant_under_inference_->entry_astate->clone());
  }

  return cloned;
}

void AbductiveDomain::canonicalize() {
  if (!path_formula_ || !path_formula_->hasEqualities()) {
    return;
  }
  auto canon_access = [this](const Access &access) -> Access {
    if (access.kind == AccessKind::ArrayIndex) {
      Access out = access;
      out.index = getCanonical(access.index);
      return out;
    }
    return access;
  };

  auto canon_address = [this](const Address &a) -> Address {
    Address out(getCanonical(a.addr));
    out.history = a.history;
    return out;
  };

  // Post stack (in-place: keys are llvm::Value*, never change)
  for (auto &kv : post_stack_.getMap()) {
    AbstractValue c = getCanonical(kv.second.addr);
    if (!(c == kv.second.addr)) {
      kv.second.addr = c;
    }
  }

  // Pre stack (in-place: keys are llvm::Value*, never change)
  for (auto &kv : pre_stack_.getMap()) {
    AbstractValue c = getCanonical(kv.second.addr);
    if (!(c == kv.second.addr)) {
      kv.second.addr = c;
    }
  }

  // Post heap: fast in-place pass unless a source key or ArrayIndex key needs
  // re-keying or an empty edge bucket should be pruned.
  {
    auto &edges = post_heap_.getEdges();
    bool needs_rebuild = false;
    for (const auto &kv : edges) {
      if (kv.second.empty() || !(getCanonical(kv.first) == kv.first)) {
        needs_rebuild = true;
        break;
      }
      for (const auto &edge_kv : kv.second) {
        if (edge_kv.first.kind == AccessKind::ArrayIndex &&
            !(getCanonical(edge_kv.first.index) == edge_kv.first.index)) {
          needs_rebuild = true;
          break;
        }
      }
      if (needs_rebuild)
        break;
    }
    if (!needs_rebuild) {
      for (auto &kv : edges) {
        for (auto &edge_kv : kv.second) {
          AbstractValue c = getCanonical(edge_kv.second.addr);
          if (!(c == edge_kv.second.addr)) {
            edge_kv.second.addr = c;
          }
        }
      }
    } else {
      Heap new_heap;
      for (const auto &kv : edges) {
        AbstractValue from = getCanonical(kv.first);
        if (kv.second.empty()) {
          continue;
        }
        for (const auto &edge_kv : kv.second) {
          Access acc = canon_access(edge_kv.first);
          Address to = canon_address(edge_kv.second);
          if (!new_heap.findEdge(from, acc)) {
            new_heap.addEdge(from, acc, to);
          }
        }
      }
      post_heap_ = std::move(new_heap);
    }
  }

  // Pre heap (preserve registered addresses even when edge maps are empty!)
  {
    auto &edges = pre_heap_.getEdges();
    bool needs_rebuild = false;
    for (const auto &kv : edges) {
      if (!(getCanonical(kv.first) == kv.first)) {
        needs_rebuild = true;
        break;
      }
      for (const auto &edge_kv : kv.second) {
        if (edge_kv.first.kind == AccessKind::ArrayIndex &&
            !(getCanonical(edge_kv.first.index) == edge_kv.first.index)) {
          needs_rebuild = true;
          break;
        }
        AbstractValue to_c = getCanonical(edge_kv.second.addr);
        if (edges.find(to_c) == edges.end()) {
          needs_rebuild = true;
          break;
        }
      }
      if (needs_rebuild)
        break;
    }
    if (!needs_rebuild) {
      for (auto &kv : edges) {
        for (auto &edge_kv : kv.second) {
          AbstractValue c = getCanonical(edge_kv.second.addr);
          if (!(c == edge_kv.second.addr)) {
            edge_kv.second.addr = c;
          }
        }
      }
    } else {
      Heap new_heap;
      for (const auto &kv : edges) {
        AbstractValue from = getCanonical(kv.first);
        new_heap.registerAddress(from);
        for (const auto &edge_kv : kv.second) {
          Access acc = canon_access(edge_kv.first);
          Address to = canon_address(edge_kv.second);
          if (!new_heap.findEdge(from, acc)) {
            new_heap.addEdge(from, acc, to);
          }
          new_heap.registerAddress(to.addr);
        }
      }
      pre_heap_ = std::move(new_heap);
    }
  }

  auto anyKeyNonCanon = [this](const auto &m) -> bool {
    for (const auto &kv : m) {
      if (!(getCanonical(kv.first) == kv.first))
        return true;
    }
    return false;
  };
  auto anyElemNonCanon = [this](const auto &s) -> bool {
    for (AbstractValue v : s) {
      if (!(getCanonical(v) == v))
        return true;
    }
    return false;
  };

  // Post attrs
  if (!post_attrs_.getAttrs().empty() &&
      anyKeyNonCanon(post_attrs_.getAttrs())) {
    AddressAttributes new_attrs;
    for (const auto &kv : post_attrs_.getAttrs()) {
      AbstractValue addr = getCanonical(kv.first);
      for (Attribute a : kv.second) {
        new_attrs.add(addr, a);
      }
    }
    post_attrs_ = std::move(new_attrs);
  }

  // Pre attrs
  if (!pre_attrs_.getAttrs().empty() && anyKeyNonCanon(pre_attrs_.getAttrs())) {
    AddressAttributes new_attrs;
    for (const auto &kv : pre_attrs_.getAttrs()) {
      AbstractValue addr = getCanonical(kv.first);
      for (Attribute a : kv.second) {
        new_attrs.add(addr, a);
      }
    }
    pre_attrs_ = std::move(new_attrs);
  }

  // Pre MustBeValid (keep earliest timestamp)
  if (!pre_must_be_valid_.empty() && anyKeyNonCanon(pre_must_be_valid_)) {
    std::map<AbstractValue, MustBeValidInfo> canon_map;
    for (const auto &kv : pre_must_be_valid_) {
      AbstractValue addr = getCanonical(kv.first);
      auto it = canon_map.find(addr);
      if (it == canon_map.end() || kv.second.timestamp < it->second.timestamp) {
        canon_map[addr] = kv.second;
      }
    }
    pre_must_be_valid_ = std::move(canon_map);
  }

  // Pre MustBeInitialized (keep earliest timestamp)
  if (!pre_must_be_initialized_.empty() &&
      anyKeyNonCanon(pre_must_be_initialized_)) {
    std::map<AbstractValue, MustBeInitializedInfo> canon_map;
    for (const auto &kv : pre_must_be_initialized_) {
      AbstractValue addr = getCanonical(kv.first);
      auto it = canon_map.find(addr);
      if (it == canon_map.end() || kv.second.timestamp < it->second.timestamp) {
        canon_map[addr] = kv.second;
      }
    }
    pre_must_be_initialized_ = std::move(canon_map);
  }

  // Pre MustNotBeTainted
  if (!pre_must_not_be_tainted_.empty() &&
      anyKeyNonCanon(pre_must_not_be_tainted_)) {
    std::map<AbstractValue, std::vector<MustNotBeTaintedSink>> canon_map;
    for (const auto &kv : pre_must_not_be_tainted_) {
      AbstractValue addr = getCanonical(kv.first);
      auto &dst = canon_map[addr];
      dst.insert(dst.end(), kv.second.begin(), kv.second.end());
    }
    pre_must_not_be_tainted_ = std::move(canon_map);
  }

  // Post WrittenTo (keep latest timestamp)
  if (!post_written_to_.empty() && anyKeyNonCanon(post_written_to_)) {
    std::map<AbstractValue, WrittenToInfo> canon_map;
    for (const auto &kv : post_written_to_) {
      AbstractValue addr = getCanonical(kv.first);
      auto it = canon_map.find(addr);
      if (it == canon_map.end() || kv.second.timestamp > it->second.timestamp) {
        canon_map[addr] = kv.second;
      }
    }
    post_written_to_ = std::move(canon_map);
  }

  // Post AllocationInfo
  if (!post_allocation_info_.empty() && anyKeyNonCanon(post_allocation_info_)) {
    std::map<AbstractValue, AllocationInfo> canon_map;
    for (const auto &kv : post_allocation_info_) {
      AbstractValue addr = getCanonical(kv.first);
      canon_map.emplace(addr, kv.second);
    }
    post_allocation_info_ = std::move(canon_map);
  }

  // Post PropagateTaintFrom
  if (!post_propagate_taint_from_.empty()) {
    if (!anyKeyNonCanon(post_propagate_taint_from_)) {
      for (auto &kv : post_propagate_taint_from_) {
        for (auto &info : kv.second) {
          for (AbstractValue &src : info.sources) {
            src = getCanonical(src);
          }
        }
      }
    } else {
      std::map<AbstractValue, std::vector<PropagateTaintFromInfo>> canon_map;
      for (const auto &kv : post_propagate_taint_from_) {
        AbstractValue addr = getCanonical(kv.first);
        for (PropagateTaintFromInfo info : kv.second) {
          for (AbstractValue &src : info.sources) {
            src = getCanonical(src);
          }
          canon_map[addr].push_back(std::move(info));
        }
      }
      post_propagate_taint_from_ = std::move(canon_map);
    }
  }

  // Post UnknownEffect
  if (!post_unknown_effect_.empty() && anyKeyNonCanon(post_unknown_effect_)) {
    std::map<AbstractValue, UnknownEffectInfo> canon_map;
    for (const auto &kv : post_unknown_effect_) {
      canon_map.emplace(getCanonical(kv.first), kv.second);
    }
    post_unknown_effect_ = std::move(canon_map);
  }

  // Post TaintSanitized
  if (!post_taint_sanitized_.empty() && anyKeyNonCanon(post_taint_sanitized_)) {
    std::map<AbstractValue, std::vector<SanitizerRecord>> canon_map;
    for (const auto &kv : post_taint_sanitized_) {
      AbstractValue addr = getCanonical(kv.first);
      auto &dst = canon_map[addr];
      dst.insert(dst.end(), kv.second.begin(), kv.second.end());
    }
    post_taint_sanitized_ = std::move(canon_map);
  }

  // Dynamic types & decompiler & auxiliary maps
  if (!dynamic_types_.empty() && anyKeyNonCanon(dynamic_types_)) {
    std::map<AbstractValue, std::string> canon_types;
    for (const auto &kv : dynamic_types_) {
      canon_types.emplace(getCanonical(kv.first), kv.second);
    }
    dynamic_types_ = std::move(canon_types);
  }

  if (!decompiler_.empty() && anyKeyNonCanon(decompiler_)) {
    std::map<AbstractValue, std::string> canon_decomp;
    for (const auto &kv : decompiler_) {
      canon_decomp.emplace(getCanonical(kv.first), kv.second);
    }
    decompiler_ = std::move(canon_decomp);
  }

  for (auto &edge : recursive_call_edges_) {
    for (AbstractValue &arg : edge.args) {
      arg = getCanonical(arg);
    }
  }

  if (!symbolic_allocation_sizes_.empty()) {
    if (!anyKeyNonCanon(symbolic_allocation_sizes_)) {
      for (auto &kv : symbolic_allocation_sizes_) {
        kv.second = getCanonical(kv.second);
      }
    } else {
      std::map<AbstractValue, AbstractValue> canon_sym_sizes;
      for (const auto &kv : symbolic_allocation_sizes_) {
        canon_sym_sizes.emplace(getCanonical(kv.first),
                                getCanonical(kv.second));
      }
      symbolic_allocation_sizes_ = std::move(canon_sym_sizes);
    }
  }

  if (!shared_ptr_targets_.empty() && anyElemNonCanon(shared_ptr_targets_)) {
    std::set<AbstractValue> canon_sptr;
    for (AbstractValue v : shared_ptr_targets_) {
      canon_sptr.insert(getCanonical(v));
    }
    shared_ptr_targets_ = std::move(canon_sptr);
  }

  // Taint domain
  if (!taint_domain_.getMap().empty() &&
      anyKeyNonCanon(taint_domain_.getMap())) {
    TaintDomain new_taint;
    for (const auto &kv : taint_domain_.getMap()) {
      AbstractValue addr = getCanonical(kv.first);
      for (const auto &item : kv.second) {
        new_taint.add(addr, item);
      }
    }
    taint_domain_ = std::move(new_taint);
  }

  // Invalidation info
  if (!invalidation_info_.empty() && anyKeyNonCanon(invalidation_info_)) {
    std::map<AbstractValue,
             std::pair<InvalidationKind, const llvm::Instruction *>>
        new_info;
    for (const auto &kv : invalidation_info_) {
      AbstractValue addr = getCanonical(kv.first);
      if (new_info.find(addr) == new_info.end()) {
        new_info.insert({addr, kv.second});
      }
    }
    invalidation_info_ = std::move(new_info);
  }

  // Allocation sizes
  if (!allocation_sizes_.empty() && anyKeyNonCanon(allocation_sizes_)) {
    std::map<AbstractValue, uint64_t> new_sizes;
    for (const auto &kv : allocation_sizes_) {
      AbstractValue addr = getCanonical(kv.first);
      auto it = new_sizes.find(addr);
      if (it == new_sizes.end()) {
        new_sizes.emplace(addr, kv.second);
      } else {
        it->second = std::min(it->second, kv.second);
      }
    }
    allocation_sizes_ = std::move(new_sizes);
  }

  // Allocation roots
  if (!allocation_roots_.empty()) {
    if (!anyKeyNonCanon(allocation_roots_)) {
      for (auto &kv : allocation_roots_) {
        kv.second = getCanonical(kv.second);
      }
    } else {
      std::map<AbstractValue, AbstractValue> new_roots;
      for (const auto &kv : allocation_roots_) {
        new_roots[getCanonical(kv.first)] = getCanonical(kv.second);
      }
      allocation_roots_ = std::move(new_roots);
    }
  }

  // Dynamic type specialization set
  if (!need_dynamic_type_specialization_.empty() &&
      anyElemNonCanon(need_dynamic_type_specialization_)) {
    std::set<AbstractValue> canon_set;
    for (AbstractValue av : need_dynamic_type_specialization_) {
      canon_set.insert(getCanonical(av));
    }
    need_dynamic_type_specialization_ = std::move(canon_set);
  }
}

IncorporateResult
AbductiveDomain::incorporateNewEqs(const NewEqs &initial_eqs) {
  std::deque<NewEq> worklist(initial_eqs.begin(), initial_eqs.end());
  unsigned steps = 0;
  constexpr unsigned kMaxSteps = 4096;

  auto hasConcreteAlloc = [this](AbstractValue v) -> bool {
    return post_attrs_.has(v, Attribute::Allocated) ||
           post_attrs_.has(v, Attribute::Stack) ||
           post_attrs_.has(v, Attribute::Global) ||
           post_allocation_info_.find(v) != post_allocation_info_.end();
  };

  auto rawAllocRoot = [this](AbstractValue v) -> std::optional<AbstractValue> {
    auto it = allocation_roots_.find(v);
    if (it != allocation_roots_.end()) {
      return it->second;
    }
    if (post_attrs_.has(v, Attribute::Allocated) ||
        post_attrs_.has(v, Attribute::Stack) ||
        post_attrs_.has(v, Attribute::Global) ||
        post_allocation_info_.find(v) != post_allocation_info_.end()) {
      return v;
    }
    return std::nullopt;
  };

  bool merged_any_av = false;
  while (!worklist.empty() && steps++ < kMaxSteps) {
    NewEq eq = worklist.front();
    worklist.pop_front();

    if (eq.kind == NewEq::Kind::EqZero) {
      AbstractValue v = getCanonical(eq.lhs);
      // Stack, Global, or locally allocated non-null memory cannot be 0.
      if (post_attrs_.has(v, Attribute::Stack) ||
          post_attrs_.has(v, Attribute::Global) ||
          post_attrs_.has(v, Attribute::Allocated) ||
          post_allocation_info_.find(v) != post_allocation_info_.end()) {
        return IncorporateResult::unsat();
      }
      // If this address was required to be valid in the precondition and is now
      // constrained to zero (e.g., `*p = 1; if (!p) {}`), emit
      // PotentialInvalidAccess (Infer's LatentInvalidAccess).
      auto mbv_it = pre_must_be_valid_.find(v);
      if (mbv_it != pre_must_be_valid_.end()) {
        return IncorporateResult::potentialInvalidAccess(v, mbv_it->second);
      }
      continue;
    }

    // NewEq::Kind::Equal (lhs -> rhs)
    AbstractValue v_old = eq.lhs;
    AbstractValue v_new = getCanonical(eq.rhs);
    if (v_old == v_new) {
      continue;
    }
    merged_any_av = true;

    // Two distinct concrete allocations cannot alias (UNSAT).
    // Compare raw pre-merge allocation roots because path_formula_ has already
    // unified v_old -> v_new before incorporateNewEqs runs.
    auto r_old = rawAllocRoot(v_old);
    auto r_new = rawAllocRoot(v_new);
    if (r_old && r_new && !(*r_old == *r_new)) {
      return IncorporateResult::unsat();
    }

    // Merge post_heap_ edges from v_old into v_new, equating colliding targets.
    {
      auto it = post_heap_.getEdges().find(v_old);
      if (it != post_heap_.getEdges().end()) {
        auto edges_copy = it->second;
        post_heap_.removeEdges(v_old);
        for (const auto &edge_kv : edges_copy) {
          if (auto *existing = post_heap_.findEdge(v_new, edge_kv.first)) {
            AbstractValue t1 = getCanonical(existing->addr);
            AbstractValue t2 = getCanonical(edge_kv.second.addr);
            if (!(t1 == t2)) {
              if (!getPathFormula().addEquality(t1, t2)) {
                return IncorporateResult::unsat();
              }
              for (const auto &ne : getPathFormula().takeNewEqs()) {
                worklist.push_back(ne);
              }
            }
          } else {
            post_heap_.addEdge(v_new, edge_kv.first, edge_kv.second);
          }
        }
      }
    }

    // Merge pre_heap_ edges from v_old into v_new.
    {
      auto it = pre_heap_.getEdges().find(v_old);
      if (it != pre_heap_.getEdges().end()) {
        auto edges_copy = it->second;
        pre_heap_.removeEdges(v_old);
        pre_heap_.registerAddress(v_new);
        for (const auto &edge_kv : edges_copy) {
          if (auto *existing = pre_heap_.findEdge(v_new, edge_kv.first)) {
            AbstractValue t1 = getCanonical(existing->addr);
            AbstractValue t2 = getCanonical(edge_kv.second.addr);
            if (!(t1 == t2)) {
              if (!getPathFormula().addEquality(t1, t2)) {
                return IncorporateResult::unsat();
              }
              for (const auto &ne : getPathFormula().takeNewEqs()) {
                worklist.push_back(ne);
              }
            }
          } else {
            pre_heap_.addEdge(v_new, edge_kv.first, edge_kv.second);
          }
        }
      }
    }

    // Merge attributes and parameterized metadata.
    for (Attribute a : post_attrs_.get(v_old)) {
      post_attrs_.add(v_new, a);
    }
    post_attrs_.clear(v_old);

    for (Attribute a : pre_attrs_.get(v_old)) {
      pre_attrs_.add(v_new, a);
    }
    pre_attrs_.clear(v_old);

    auto mbv_old = pre_must_be_valid_.find(v_old);
    if (mbv_old != pre_must_be_valid_.end()) {
      auto mbv_new = pre_must_be_valid_.find(v_new);
      if (mbv_new == pre_must_be_valid_.end() ||
          mbv_old->second.timestamp < mbv_new->second.timestamp) {
        pre_must_be_valid_[v_new] = mbv_old->second;
      }
      pre_must_be_valid_.erase(mbv_old);
    }

    auto mbi_old = pre_must_be_initialized_.find(v_old);
    if (mbi_old != pre_must_be_initialized_.end()) {
      auto mbi_new = pre_must_be_initialized_.find(v_new);
      if (mbi_new == pre_must_be_initialized_.end() ||
          mbi_old->second.timestamp < mbi_new->second.timestamp) {
        pre_must_be_initialized_[v_new] = mbi_old->second;
      }
      pre_must_be_initialized_.erase(mbi_old);
    }

    auto mnt_old = pre_must_not_be_tainted_.find(v_old);
    if (mnt_old != pre_must_not_be_tainted_.end()) {
      auto &dst = pre_must_not_be_tainted_[v_new];
      dst.insert(dst.end(), mnt_old->second.begin(), mnt_old->second.end());
      pre_must_not_be_tainted_.erase(mnt_old);
    }

    auto wt_old = post_written_to_.find(v_old);
    if (wt_old != post_written_to_.end()) {
      auto wt_new = post_written_to_.find(v_new);
      if (wt_new == post_written_to_.end() ||
          wt_old->second.timestamp > wt_new->second.timestamp) {
        post_written_to_[v_new] = wt_old->second;
      }
      post_written_to_.erase(wt_old);
    }

    auto ai_old = post_allocation_info_.find(v_old);
    if (ai_old != post_allocation_info_.end()) {
      post_allocation_info_.emplace(v_new, ai_old->second);
      post_allocation_info_.erase(ai_old);
    }

    auto ue_old = post_unknown_effect_.find(v_old);
    if (ue_old != post_unknown_effect_.end()) {
      post_unknown_effect_.emplace(v_new, ue_old->second);
      post_unknown_effect_.erase(ue_old);
    }

    auto inv_old = invalidation_info_.find(v_old);
    if (inv_old != invalidation_info_.end()) {
      invalidation_info_.emplace(v_new, inv_old->second);
      invalidation_info_.erase(inv_old);
    }

    auto sz_old = allocation_sizes_.find(v_old);
    if (sz_old != allocation_sizes_.end()) {
      auto sz_new = allocation_sizes_.find(v_new);
      if (sz_new == allocation_sizes_.end()) {
        allocation_sizes_[v_new] = sz_old->second;
      } else {
        sz_new->second = std::min(sz_new->second, sz_old->second);
      }
      allocation_sizes_.erase(sz_old);
    }

    auto dt_old = dynamic_types_.find(v_old);
    if (dt_old != dynamic_types_.end()) {
      auto dt_new = dynamic_types_.find(v_new);
      if (dt_new != dynamic_types_.end() && dt_new->second != dt_old->second) {
        return IncorporateResult::unsat();
      }
      dynamic_types_[v_new] = dt_old->second;
      dynamic_types_.erase(dt_old);
    }

    // Check if the merged value is now simultaneously null and allocated/valid.
    if (getPathFormula().isNull(v_new)) {
      if (hasConcreteAlloc(v_new)) {
        return IncorporateResult::unsat();
      }
      auto mbv_it = pre_must_be_valid_.find(v_new);
      if (mbv_it != pre_must_be_valid_.end()) {
        return IncorporateResult::potentialInvalidAccess(v_new, mbv_it->second);
      }
    }
  }

  if (merged_any_av) {
    canonicalize();
  }
  return IncorporateResult::ok();
}

IncorporateResult AbductiveDomain::incorporatePendingNewEqs() {
  NewEqs eqs = getPathFormula().takeNewEqs();
  if (eqs.empty()) {
    return IncorporateResult::ok();
  }
  return incorporateNewEqs(eqs);
}

bool AbductiveDomain::leq(const AbductiveDomain &lhs,
                          const AbductiveDomain &rhs) {
  // Infer's GraphComparison.is_isograph: check structural bijection from lhs to
  // rhs over stacks, pre/post heaps, attributes, and path formulas.
  if (!(lhs.topl_history_ == rhs.topl_history_)) {
    return false;
  }
  if (lhs.skipped_calls_ != rhs.skipped_calls_) {
    return false;
  }
  if (lhs.unknown_values_ != rhs.unknown_values_) {
    return false;
  }

  const auto &l_post_stack = lhs.post_stack_.getMap();
  const auto &r_post_stack = rhs.post_stack_.getMap();
  if (l_post_stack.size() != r_post_stack.size()) {
    return false;
  }

  const auto &l_pre_stack = lhs.pre_stack_.getMap();
  const auto &r_pre_stack = rhs.pre_stack_.getMap();
  if (l_pre_stack.size() != r_pre_stack.size()) {
    return false;
  }

  std::map<AbstractValue, AbstractValue> l_to_r;
  std::map<AbstractValue, AbstractValue> r_to_l;
  std::queue<std::pair<AbstractValue, AbstractValue>> worklist;

  auto bind_pair = [&](AbstractValue l_raw, AbstractValue r_raw) -> bool {
    AbstractValue l = lhs.getCanonical(l_raw);
    AbstractValue r = rhs.getCanonical(r_raw);
    auto it_l = l_to_r.find(l);
    auto it_r = r_to_l.find(r);
    if (it_l != l_to_r.end() || it_r != r_to_l.end()) {
      return it_l != l_to_r.end() && it_r != r_to_l.end() &&
             it_l->second == r && it_r->second == l;
    }
    l_to_r.emplace(l, r);
    r_to_l.emplace(r, l);
    worklist.emplace(l, r);
    return true;
  };

  for (const auto &kv : l_post_stack) {
    auto r_it = r_post_stack.find(kv.first);
    if (r_it == r_post_stack.end()) {
      return false;
    }
    if (!bind_pair(kv.second.addr, r_it->second.addr)) {
      return false;
    }
  }
  for (const auto &kv : l_pre_stack) {
    auto r_it = r_pre_stack.find(kv.first);
    if (r_it == r_pre_stack.end()) {
      return false;
    }
    if (!bind_pair(kv.second.addr, r_it->second.addr)) {
      return false;
    }
  }

  auto match_heap_edges = [&](const Heap &lh, const Heap &rh, AbstractValue l,
                              AbstractValue r) -> bool {
    auto l_it = lh.getEdges().find(l);
    auto r_it = rh.getEdges().find(r);
    const bool l_empty = (l_it == lh.getEdges().end() || l_it->second.empty());
    const bool r_empty = (r_it == rh.getEdges().end() || r_it->second.empty());
    if (l_empty && r_empty) {
      return true;
    }
    if (l_empty != r_empty) {
      return false;
    }
    if (l_it->second.size() != r_it->second.size()) {
      return false;
    }
    for (const auto &le : l_it->second) {
      Access expected = le.first;
      if (expected.kind == AccessKind::ArrayIndex) {
        AbstractValue l_idx = lhs.getCanonical(expected.index);
        auto map_it = l_to_r.find(l_idx);
        if (map_it != l_to_r.end()) {
          expected.index = map_it->second;
        } else {
          // Try matching by constant value or unique compatible edge.
          bool matched = false;
          auto l_const = lhs.getPathFormula().asConstantInt(l_idx);
          for (const auto &re : r_it->second) {
            if (re.first.kind == AccessKind::ArrayIndex &&
                re.first.stride_bytes == expected.stride_bytes) {
              AbstractValue r_idx = rhs.getCanonical(re.first.index);
              auto r_const = rhs.getPathFormula().asConstantInt(r_idx);
              if (l_const == r_const && bind_pair(l_idx, r_idx) &&
                  bind_pair(le.second.addr, re.second.addr)) {
                matched = true;
                break;
              }
            }
          }
          if (!matched) {
            return false;
          }
          continue;
        }
      }
      auto re_it = r_it->second.find(expected);
      if (re_it == r_it->second.end()) {
        return false;
      }
      if (!bind_pair(le.second.addr, re_it->second.addr)) {
        return false;
      }
    }
    return true;
  };

  while (!worklist.empty()) {
    auto [l, r] = worklist.front();
    worklist.pop();

    // Check attributes on matched addresses.
    if (lhs.post_attrs_.get(l) != rhs.post_attrs_.get(r)) {
      return false;
    }
    if (lhs.pre_attrs_.get(l) != rhs.pre_attrs_.get(r)) {
      return false;
    }
    if ((lhs.pre_must_be_valid_.count(l) > 0) !=
        (rhs.pre_must_be_valid_.count(r) > 0)) {
      return false;
    }
    if ((lhs.pre_must_be_initialized_.count(l) > 0) !=
        (rhs.pre_must_be_initialized_.count(r) > 0)) {
      return false;
    }
    if ((lhs.post_written_to_.count(l) > 0) !=
        (rhs.post_written_to_.count(r) > 0)) {
      return false;
    }
    if (lhs.getAllocationSize(l) != rhs.getAllocationSize(r)) {
      return false;
    }
    if (lhs.getDynamicType(l) != rhs.getDynamicType(r)) {
      return false;
    }
    if (lhs.taint_domain_.has(l) != rhs.taint_domain_.has(r)) {
      return false;
    }
    auto l_inv = lhs.getInvalidationInfo(l);
    auto r_inv = rhs.getInvalidationInfo(r);
    if (l_inv.has_value() != r_inv.has_value()) {
      return false;
    }
    if (l_inv && r_inv && l_inv->first != r_inv->first) {
      return false;
    }

    if (!match_heap_edges(lhs.post_heap_, rhs.post_heap_, l, r)) {
      return false;
    }
    if (!match_heap_edges(lhs.pre_heap_, rhs.pre_heap_, l, r)) {
      return false;
    }
  }

  // Compare path formulas under the discovered bijection.
  Substitution subst;
  for (const auto &kv : l_to_r) {
    subst.add(kv.first, kv.second);
  }
  PulseFormula mapped_lhs = lhs.getPathFormula().applySubstitution(subst);
  return mapped_lhs.equivalentTo(rhs.getPathFormula());
}

std::optional<AbductiveDomain>
AbductiveDomain::merge(const AbductiveDomain &d1, const AbductiveDomain &d2) {
  if (!(d1.topl_history_ == d2.topl_history_))
    return std::nullopt;

  PulseFormula merged_formula;
  if (d1.path_formula_ && d2.path_formula_) {
    merged_formula = PulseFormula::join(*d1.path_formula_, *d2.path_formula_);
  } else if (d1.path_formula_) {
    merged_formula = d1.path_formula_->clone();
  } else if (d2.path_formula_) {
    merged_formula = d2.path_formula_->clone();
  }

  if (!merged_formula.isConsistent()) {
    return std::nullopt;
  }

  AbductiveDomain merged;
  merged.topl_history_ = d1.topl_history_;
  merged.path_formula_ =
      std::make_unique<PulseFormula>(std::move(merged_formula));

  for (const auto &kv : d1.post_stack_.getMap()) {
    merged.post_stack_.add(kv.first, kv.second);
  }
  for (const auto &kv : d2.post_stack_.getMap()) {
    if (!merged.post_stack_.find(kv.first)) {
      merged.post_stack_.add(kv.first, kv.second);
    }
  }

  for (const auto &kv : d1.post_heap_.getEdges()) {
    for (const auto &edge_kv : kv.second) {
      merged.post_heap_.addEdge(kv.first, edge_kv.first, edge_kv.second);
    }
  }
  for (const auto &kv : d2.post_heap_.getEdges()) {
    for (const auto &edge_kv : kv.second) {
      if (!merged.post_heap_.findEdge(kv.first, edge_kv.first)) {
        merged.post_heap_.addEdge(kv.first, edge_kv.first, edge_kv.second);
      }
    }
  }

  for (const auto &kv : d1.post_attrs_.getAttrs()) {
    for (Attribute attr : kv.second) {
      merged.post_attrs_.add(kv.first, attr);
    }
  }
  for (const auto &kv : d2.post_attrs_.getAttrs()) {
    for (Attribute attr : kv.second) {
      merged.post_attrs_.add(kv.first, attr);
    }
  }

  for (const auto &kv : d1.pre_stack_.getMap()) {
    merged.pre_stack_.add(kv.first, kv.second);
  }
  for (const auto &kv : d2.pre_stack_.getMap()) {
    if (!merged.pre_stack_.find(kv.first)) {
      merged.pre_stack_.add(kv.first, kv.second);
    }
  }

  for (const auto &kv : d1.pre_heap_.getEdges()) {
    merged.pre_heap_.registerAddress(kv.first);
    for (const auto &edge_kv : kv.second) {
      merged.pre_heap_.addEdge(kv.first, edge_kv.first, edge_kv.second);
    }
  }
  for (const auto &kv : d2.pre_heap_.getEdges()) {
    merged.pre_heap_.registerAddress(kv.first);
    for (const auto &edge_kv : kv.second) {
      if (!merged.pre_heap_.findEdge(kv.first, edge_kv.first)) {
        merged.pre_heap_.addEdge(kv.first, edge_kv.first, edge_kv.second);
      }
    }
  }

  for (const auto &kv : d1.pre_attrs_.getAttrs()) {
    for (Attribute attr : kv.second) {
      merged.pre_attrs_.add(kv.first, attr);
    }
  }
  for (const auto &kv : d2.pre_attrs_.getAttrs()) {
    for (Attribute attr : kv.second) {
      merged.pre_attrs_.add(kv.first, attr);
    }
  }

  merged.pre_must_be_valid_ = d1.pre_must_be_valid_;
  for (const auto &kv : d2.pre_must_be_valid_) {
    merged.pre_must_be_valid_.emplace(kv.first, kv.second);
  }
  merged.pre_must_be_initialized_ = d1.pre_must_be_initialized_;
  for (const auto &kv : d2.pre_must_be_initialized_) {
    merged.pre_must_be_initialized_.emplace(kv.first, kv.second);
  }
  merged.pre_must_not_be_tainted_ = d1.pre_must_not_be_tainted_;
  for (const auto &kv : d2.pre_must_not_be_tainted_) {
    auto &dst = merged.pre_must_not_be_tainted_[kv.first];
    dst.insert(dst.end(), kv.second.begin(), kv.second.end());
  }

  merged.post_written_to_ = d1.post_written_to_;
  for (const auto &kv : d2.post_written_to_) {
    auto it = merged.post_written_to_.find(kv.first);
    if (it == merged.post_written_to_.end() ||
        kv.second.timestamp > it->second.timestamp) {
      merged.post_written_to_[kv.first] = kv.second;
    }
  }
  merged.post_allocation_info_ = d1.post_allocation_info_;
  for (const auto &kv : d2.post_allocation_info_) {
    merged.post_allocation_info_.emplace(kv.first, kv.second);
  }
  merged.post_propagate_taint_from_ = d1.post_propagate_taint_from_;
  for (const auto &kv : d2.post_propagate_taint_from_) {
    auto &dst = merged.post_propagate_taint_from_[kv.first];
    dst.insert(dst.end(), kv.second.begin(), kv.second.end());
  }
  merged.post_unknown_effect_ = d1.post_unknown_effect_;
  for (const auto &kv : d2.post_unknown_effect_) {
    merged.post_unknown_effect_.emplace(kv.first, kv.second);
  }
  merged.post_taint_sanitized_ = d1.post_taint_sanitized_;
  for (const auto &kv : d2.post_taint_sanitized_) {
    auto &dst = merged.post_taint_sanitized_[kv.first];
    dst.insert(dst.end(), kv.second.begin(), kv.second.end());
  }
  merged.dynamic_types_ = d1.dynamic_types_;
  for (const auto &kv : d2.dynamic_types_) {
    merged.dynamic_types_.emplace(kv.first, kv.second);
  }
  merged.decompiler_ = d1.decompiler_;
  for (const auto &kv : d2.decompiler_) {
    merged.decompiler_.emplace(kv.first, kv.second);
  }
  merged.taint_domain_.join(d1.taint_domain_);
  merged.taint_domain_.join(d2.taint_domain_);

  for (const auto &kv : d1.invalidation_info_)
    merged.invalidation_info_.insert(kv);
  for (const auto &kv : d2.invalidation_info_) {
    merged.invalidation_info_.emplace(kv.first, kv.second);
  }

  merged.allocation_sizes_ = d1.allocation_sizes_;
  for (const auto &kv : d2.allocation_sizes_) {
    auto it = merged.allocation_sizes_.find(kv.first);
    if (it == merged.allocation_sizes_.end()) {
      merged.allocation_sizes_.emplace(kv.first, kv.second);
    } else {
      it->second = std::min(it->second, kv.second);
    }
  }

  merged.allocation_roots_ = d1.allocation_roots_;
  for (const auto &kv : d2.allocation_roots_) {
    merged.allocation_roots_.emplace(kv.first, kv.second);
  }

  merged.skipped_calls_ = d1.skipped_calls_;
  merged.skipped_calls_.insert(d2.skipped_calls_.begin(),
                               d2.skipped_calls_.end());

  merged.transitive_info_ =
      TransitiveInfo::merge(d1.transitive_info_, d2.transitive_info_);
  merged.unknown_values_ = d1.unknown_values_ || d2.unknown_values_;

  merged.need_dynamic_type_specialization_ =
      d1.need_dynamic_type_specialization_;
  merged.need_dynamic_type_specialization_.insert(
      d2.need_dynamic_type_specialization_.begin(),
      d2.need_dynamic_type_specialization_.end());

  merged.recursive_calls_ = d1.recursive_calls_;
  merged.recursive_calls_.insert(d2.recursive_calls_.begin(),
                                 d2.recursive_calls_.end());

  merged.loop_header_info_ = d1.loop_header_info_;
  merged.loop_path_stamps_ = d1.loop_path_stamps_;
  merged.timestamp_counter_ =
      std::max(d1.timestamp_counter_, d2.timestamp_counter_);

  if (d1.loop_invariant_under_inference_) {
    merged.loop_invariant_under_inference_ =
        std::make_unique<LoopInvariantUnderInference>(
            d1.loop_invariant_under_inference_->header,
            d1.loop_invariant_under_inference_->entry_astate->clone());
  }

  merged.canonicalize();
  return merged;
}

} // namespace pulse
