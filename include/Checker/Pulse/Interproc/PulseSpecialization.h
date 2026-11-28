/** @file PulseSpecialization.h @brief Function specialization for Pulse interprocedural analysis. */
#pragma once

#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Interproc/PulseSummary.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace pulse {

class AbstractValueFactory;

/**
 * HeapPath: represents a path through the heap starting from a formal
 * parameter or global variable (e.g., arg0->field1->* ).
 * Aligned with Infer's Specialization.HeapPath.
 */
class HeapPath {
public:
  enum class PathElement {
    Pvar,        // Starting from a formal parameter or global variable
    FieldAccess, // Field access: .field
    Dereference, // Pointer dereference: *
    ArrayIndex   // Array index: [i]
  };

  struct Element {
    PathElement kind;
    int param_index{-1};                  // >= 0 for formal parameter Pvar
    const llvm::Value *pvar_root{nullptr}; // Non-null for global Pvar
    unsigned field_idx{0};                // For FieldAccess
    uint64_t field_byte_offset{0};        // Byte offset for FieldAccess
    std::string field_name;               // For Pvar name or FieldAccess
    AbstractValue index;                  // For ArrayIndex
    std::optional<int64_t> const_index;   // For constant ArrayIndex
    uint64_t stride{0};                   // For ArrayIndex byte stride

    Element(PathElement k) : kind(k) {}
    Element(PathElement k, const std::string &fname)
        : kind(k), field_name(fname) {}
    Element(PathElement k, unsigned fidx, const std::string &fname = "",
            uint64_t foff = 0)
        : kind(k), field_idx(fidx), field_byte_offset(foff),
          field_name(fname) {}
    Element(PathElement k, AbstractValue idx, uint64_t s = 0)
        : kind(k), index(idx), stride(s) {}
    static Element constArrayIndex(int64_t idx, uint64_t s = 0) {
      Element e(PathElement::ArrayIndex);
      e.const_index = idx;
      e.stride = s;
      return e;
    }

    static Element formalPvar(unsigned idx, const std::string &name = "") {
      Element e(PathElement::Pvar);
      e.param_index = static_cast<int>(idx);
      e.field_name = name.empty() ? ("arg" + std::to_string(idx)) : name;
      return e;
    }

    static Element globalPvar(const llvm::Value *gv,
                              const std::string &name = "") {
      Element e(PathElement::Pvar);
      e.pvar_root = gv;
      e.field_name = name;
      return e;
    }

    bool operator<(const Element &other) const {
      if (kind != other.kind)
        return kind < other.kind;
      if (kind == PathElement::Pvar) {
        if (param_index != other.param_index)
          return param_index < other.param_index;
        if (pvar_root != other.pvar_root)
          return pvar_root < other.pvar_root;
        return field_name < other.field_name;
      }
      if (kind == PathElement::FieldAccess) {
        if (field_byte_offset != other.field_byte_offset)
          return field_byte_offset < other.field_byte_offset;
        if (field_idx != other.field_idx)
          return field_idx < other.field_idx;
        return field_name < other.field_name;
      }
      if (kind == PathElement::ArrayIndex) {
        if (stride != other.stride)
          return stride < other.stride;
        if (const_index.has_value() != other.const_index.has_value())
          return const_index.has_value() < other.const_index.has_value();
        if (const_index.has_value())
          return *const_index < *other.const_index;
        return index < other.index;
      }
      return false;
    }

    bool operator==(const Element &other) const {
      if (kind != other.kind)
        return false;
      if (kind == PathElement::Pvar)
        return param_index == other.param_index &&
               pvar_root == other.pvar_root && field_name == other.field_name;
      if (kind == PathElement::FieldAccess)
        return field_idx == other.field_idx &&
               field_byte_offset == other.field_byte_offset &&
               field_name == other.field_name;
      if (kind == PathElement::ArrayIndex) {
        if (stride != other.stride ||
            const_index.has_value() != other.const_index.has_value())
          return false;
        if (const_index.has_value())
          return *const_index == *other.const_index;
        return index == other.index;
      }
      return true;
    }
  };

private:
  std::vector<Element> path_;

public:
  HeapPath() = default;
  explicit HeapPath(const std::vector<Element> &p) : path_(p) {}

  static HeapPath fromFormal(unsigned param_idx,
                             const std::string &name = "") {
    HeapPath hp;
    hp.push(Element::formalPvar(param_idx, name));
    return hp;
  }

  static HeapPath fromGlobal(const llvm::Value *gv,
                             const std::string &name = "") {
    HeapPath hp;
    hp.push(Element::globalPvar(gv, name));
    return hp;
  }

  void push(Element e) { path_.push_back(std::move(e)); }
  void pushDeref() { path_.emplace_back(PathElement::Dereference); }
  void pushField(unsigned idx, uint64_t byte_offset = 0) {
    path_.emplace_back(PathElement::FieldAccess, idx,
                       "f" + std::to_string(idx), byte_offset);
  }
  void pushArrayIndex(AbstractValue idx, uint64_t stride = 0) {
    path_.emplace_back(PathElement::ArrayIndex, idx, stride);
  }
  void pushConstArrayIndex(int64_t idx, uint64_t stride = 0) {
    path_.push_back(Element::constArrayIndex(idx, stride));
  }

  const std::vector<Element> &getPath() const { return path_; }
  bool empty() const { return path_.empty(); }

  bool operator<(const HeapPath &other) const { return path_ < other.path_; }
  bool operator==(const HeapPath &other) const { return path_ == other.path_; }
};

/**
 * SpecializationKey: defines how a summary should be specialized.
 * Aligned with Infer's Specialization.Pulse.t (aliases + dynamic_types keyed
 * by callee HeapPaths).
 */
struct SpecializationKey {
  // Canonicalized alias equivalence classes over callee HeapPaths.
  std::vector<std::vector<HeapPath>> alias_classes;

  // Dynamic types keyed by callee HeapPaths.
  std::map<HeapPath, std::string> dynamic_types_by_path;

  // Legacy fields retained for backward compatibility.
  std::map<HeapPath, AbstractValue> heap_paths_to_values;
  std::map<AbstractValue, std::set<AbstractValue>> aliasing_map;
  std::map<AbstractValue, std::string> dynamic_types;

  void normalize();

  void merge(const SpecializationKey &other);

  static SpecializationKey
  fromAliases(std::vector<std::vector<HeapPath>> classes);

  bool operator<(const SpecializationKey &other) const {
    if (alias_classes != other.alias_classes)
      return alias_classes < other.alias_classes;
    if (dynamic_types_by_path != other.dynamic_types_by_path)
      return dynamic_types_by_path < other.dynamic_types_by_path;
    if (heap_paths_to_values != other.heap_paths_to_values)
      return heap_paths_to_values < other.heap_paths_to_values;
    if (aliasing_map != other.aliasing_map)
      return aliasing_map < other.aliasing_map;
    return dynamic_types < other.dynamic_types;
  }

  bool operator==(const SpecializationKey &other) const {
    return alias_classes == other.alias_classes &&
           dynamic_types_by_path == other.dynamic_types_by_path &&
           heap_paths_to_values == other.heap_paths_to_values &&
           aliasing_map == other.aliasing_map &&
           dynamic_types == other.dynamic_types;
  }

  bool isBottom() const {
    return alias_classes.empty() && dynamic_types_by_path.empty() &&
           heap_paths_to_values.empty() && aliasing_map.empty() &&
           dynamic_types.empty();
  }
};

/**
 * SpecializationManager: manages specialized summaries keyed by
 * (Function*, SpecializationKey).
 */
class SpecializationManager {
private:
  std::map<const llvm::Function *,
           std::map<SpecializationKey, std::unique_ptr<PulseSummary>>>
      specialized_summaries_;

  std::map<const llvm::Function *, std::set<SpecializationKey>>
      pending_specializations_;

public:
  SpecializationManager() = default;

  void addSpecializedSummary(const llvm::Function *func, SpecializationKey key,
                             std::unique_ptr<PulseSummary> summary) {
    key.normalize();
    specialized_summaries_[func][key] = std::move(summary);
    pending_specializations_[func].erase(key);
  }

  const PulseSummary *
  getSpecializedSummary(const llvm::Function *func,
                        const SpecializationKey &key) const {
    SpecializationKey norm = key;
    norm.normalize();
    auto it = specialized_summaries_.find(func);
    if (it == specialized_summaries_.end())
      return nullptr;
    auto kit = it->second.find(norm);
    if (kit == it->second.end())
      return nullptr;
    return kit->second.get();
  }

  bool hasSpecializedSummary(const llvm::Function *func,
                             const SpecializationKey &key) const {
    return getSpecializedSummary(func, key) != nullptr;
  }

  void requestSpecialization(const llvm::Function *func,
                             const SpecializationKey &key) {
    SpecializationKey norm = key;
    norm.normalize();
    if (!norm.isBottom()) {
      pending_specializations_[func].insert(std::move(norm));
    }
  }

  const std::set<SpecializationKey> &
  getPendingSpecializations(const llvm::Function *func) const {
    static const std::set<SpecializationKey> empty;
    auto it = pending_specializations_.find(func);
    return (it != pending_specializations_.end()) ? it->second : empty;
  }

  /**
   * Compute a callee-rooted SpecializationKey from call-site actuals.
   */
  static SpecializationKey
  computeSpecializationKey(const AbductiveDomain &caller_state,
                           const llvm::CallInst *call,
                           const std::vector<AbstractValue> &actual_args);

  /**
   * Apply a SpecializationKey to a callee's fresh entry state before
   * re-analyzing the callee (Infer's PulseSpecialization.apply).
   * Returns std::nullopt if the specialization assumptions are contradictory.
   */
  static std::optional<AbductiveDomain> applySpecializationToEntry(
      const AbductiveDomain &initial_astate, const llvm::Function *callee,
      const SpecializationKey &key, AbstractValueFactory &factory);

  /**
   * Legacy specialization helper on an existing domain.
   */
  static AbductiveDomain applySpecialization(const AbductiveDomain &astate,
                                             const SpecializationKey &key);
};

} // namespace pulse
