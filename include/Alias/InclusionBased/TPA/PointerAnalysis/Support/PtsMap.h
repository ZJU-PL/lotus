#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Support/PtsSet.h"

#include <type_traits>
#include <unordered_map>

namespace tpa {

// Points-to map template
//
// A PtsMap maps pointers to their points-to sets. This is used for:
// - Env: Maps top-level pointers (Pointer*) to points-to sets
// - Store: Maps memory objects (MemoryObject*) to points-to sets
//
// Template Parameter:
//   T: The key type (must be a pointer type: Pointer* or MemoryObject*)
//
// Update Strategies:
// - insert: Adds a single object to a points-to set
// - weakUpdate: Union with existing set (monotonic, never removes)
// - strongUpdate: Replaces existing set (used for definite assignments)
// - mergeWith: Merges another entire map into this one
//
// Design:
// - Used for incremental data flow analysis
// - Returns boolean indicating if the map changed (for worklist management)
template <typename T> class PtsMap {
private:
  static_assert(std::is_pointer<T>::value,
                "PtsMap only accept pointer as key type");

  // Internal mapping from key to points-to set
  using MapType = std::unordered_map<T, PtsSet>;
  MapType mapping;

public:
  using const_iterator = typename MapType::const_iterator;

  struct Binding {
    bool present;
    PtsSet value;
  };
  using ReadMap = std::unordered_map<T, Binding>;

private:
  // Used only by parallel transfer views. The base remains immutable while
  // the worker batch executes; writes stay in mapping until retirement.
  const PtsMap *base = nullptr;
  mutable ReadMap reads;

  Binding binding(T key) const {
    const auto found = mapping.find(key);
    if (found != mapping.end())
      return {true, found->second};
    if (base) {
      const auto observed = base->binding(key);
      reads.emplace(key, observed);
      return observed;
    }
    return {false, PtsSet::getEmptySet()};
  }

public:
  bool isOverlay() const { return base != nullptr; }

  PtsMap makeOverlay() const {
    PtsMap result;
    result.base = this;
    return result;
  }
  bool validateOverlay(const PtsMap &current) const {
    for (const auto &read : reads) {
      const auto actual = current.binding(read.first);
      if (actual.present != read.second.present ||
          actual.value != read.second.value)
        return false;
    }
    return true;
  }
  void commitOverlayTo(PtsMap &destination) const {
    for (const auto &write : mapping)
      destination.strongUpdate(write.first, write.second);
  }
  bool sameBindings(const PtsMap &other) const {
    if (mapping.size() != other.mapping.size())
      return false;
    for (const auto &entry : mapping) {
      const auto found = other.mapping.find(entry.first);
      if (found == other.mapping.end() || entry.second != found->second)
        return false;
    }
    return true;
  }

  // Lookup the points-to set for a key
  // Returns empty set if key not present
  PtsSet lookup(T key) const {
    assert(key != nullptr);
    return binding(key).value;
  }
  // Check if a key exists in the map
  bool contains(T key) const { return !lookup(key).empty(); }
  // Presence matters even for an empty set: first insertion can enqueue uses.
  bool hasBinding(T key) const { return binding(key).present; }

  // Insert a single memory object into a key's points-to set
  // Creates the key with empty set if not present
  // Returns true if the set changed (for worklist management)
  bool insert(T key, const MemoryObject *obj) {
    assert(key != nullptr && obj != nullptr);

    if (base) {
      const auto old = binding(key);
      const auto value = old.value.insert(obj);
      mapping.insert_or_assign(key, value);
      return value != old.value;
    }
    auto itr = mapping.find(key);
    if (itr == mapping.end())
      itr = mapping.insert(std::make_pair(key, PtsSet::getEmptySet())).first;

    auto &set = itr->second;
    auto newSet = set.insert(obj);
    if (set == newSet)
      return false;
    else {
      set = newSet;
      return true;
    }
  }

  // Weak update: union with existing set
  // Used for points-to information that flows into a variable
  // Returns true if the set changed
  bool weakUpdate(T key, PtsSet pSet) {
    assert(key != nullptr);

    if (base) {
      const auto old = binding(key);
      const auto value = old.value.merge(pSet);
      mapping.insert_or_assign(key, value);
      return !old.present || value != old.value;
    }
    auto itr = mapping.find(key);
    if (itr == mapping.end()) {
      mapping.insert(std::make_pair(key, pSet));
      return true;
    } else {
      auto &set = itr->second;
      auto newSet = set.merge(pSet);
      if (newSet == set)
        return false;
      else {
        set = newSet;
        return true;
      }
    }
  }

  // Strong update: replace existing set
  // Used when a variable is definitely assigned (not additive)
  // Returns true if the set changed
  bool strongUpdate(T key, PtsSet pSet) {
    assert(key != nullptr);

    if (base) {
      const auto old = binding(key);
      mapping.insert_or_assign(key, pSet);
      return !old.present || old.value != pSet;
    }
    auto itr = mapping.find(key);
    if (itr == mapping.end()) {
      mapping.insert(std::make_pair(key, pSet));
      return true;
    } else {
      auto &set = itr->second;
      if (set == pSet)
        return false;
      else {
        set = pSet;
        return true;
      }
    }
  }

  // Merge another map into this one (weak updates)
  // Returns true if anything changed
  bool mergeWith(const PtsMap<T> &rhs) {
    bool ret = false;
    for (auto const &mapping : rhs)
      ret |= weakUpdate(mapping.first, mapping.second);
    return ret;
  }

  size_t size() const { return mapping.size(); }
  bool empty() const { return mapping.empty(); }
  const_iterator begin() const { return mapping.begin(); }
  const_iterator end() const { return mapping.end(); }
};

} // namespace tpa
