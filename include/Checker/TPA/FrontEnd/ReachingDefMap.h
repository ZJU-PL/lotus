#pragma once

#include "Alias/InclusionBased/TPA/Util/DataStructure/VectorSet.h"

#include <unordered_map>

namespace tpa {
class MemoryObject;
} // namespace tpa

namespace tpa::taint {

template <typename NodeType> class ReachingDefStore {
private:
  using NodeSet = util::VectorSet<const NodeType *>;
  std::unordered_map<const tpa::MemoryObject *, NodeSet> store;

public:
  using const_iterator = typename decltype(store)::const_iterator;

  bool updateBinding(const tpa::MemoryObject *loc, const NodeType *node) {
    auto itr = store.find(loc);
    if (itr == store.end())
      itr = store.insert(itr, std::make_pair(loc, NodeSet()));

    auto &set = itr->second;
    if (set.size() == 1 && *set.begin() == node)
      return false;
    set.clear();
    return set.insert(node).second;
  }
  bool insertBinding(const tpa::MemoryObject *loc, const NodeType *node) {
    auto itr = store.find(loc);
    if (itr == store.end())
      itr = store.insert(itr, std::make_pair(loc, NodeSet()));

    return itr->second.insert(node).second;
  }
  bool mergeWith(const ReachingDefStore &rhs) {
    bool changed = false;
    for (auto const &mapping : rhs) {
      auto loc = mapping.first;
      auto itr = store.find(loc);
      if (itr == store.end()) {
        changed = true;
        store.insert(itr, mapping);
      } else {
        changed |= itr->second.merge(mapping.second);
      }
    }
    return changed;
  }

  // Return NULL if binding does not exist
  const NodeSet *getReachingDefs(const tpa::MemoryObject *loc) const {
    auto itr = store.find(loc);
    if (itr == store.end())
      return nullptr;
    else
      return &itr->second;
  }

  const_iterator begin() const { return store.begin(); }
  const_iterator end() const { return store.end(); }
  size_t size() const { return store.size(); }
};

template <typename NodeType> class ReachingDefMap {
private:
  using StoreType = ReachingDefStore<NodeType>;
  std::unordered_map<const NodeType *, StoreType> rdStore;

public:
  bool update(const NodeType *node, const StoreType &storeUpdate) {
    auto itr = rdStore.find(node);
    if (itr == rdStore.end()) {
      rdStore.insert(std::make_pair(node, storeUpdate));
      return true;
    } else
      return itr->second.mergeWith(storeUpdate);
  }
  StoreType &getReachingDefStore(const NodeType *node) { return rdStore[node]; }
  const StoreType &getReachingDefStore(const NodeType *node) const {
    auto itr = rdStore.find(node);
    assert(itr != rdStore.end());
    return itr->second;
  }
};

} // namespace tpa::taint
