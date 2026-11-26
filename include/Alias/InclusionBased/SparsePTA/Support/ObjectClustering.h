/**
 * @file ObjectClustering.h
 * @brief Object merging for the flow-sensitive pointer analyses.
 *
 * These are Lotus-specific abstraction policies. SVF's
 * `FlowSensitive::cluster()` instead permutes IDs for bitvector locality,
 * preserving object identity and strong-update eligibility.
 *
 * Lotus exposes object merging as an opt-in policy. Merging is sound for a
 * may-points-to analysis: coarsening the object partition only unions the
 * contents of the merged objects, and `clusterSize()` gates strong updates.
 * The policies differ in how aggressively they merge:
 *
 *  - `Footprint`: objects accessed at exactly the same graph locations. This
 *    also merges their contents, unlike the versioned solver's label sharing.
 *  - `Type`: objects with the same LLVM type and the same object kind. This is
 *    a coarse, Lotus-specific policy; it is not one of SVF's `ClusterMethod`
 *    values.
 *
 * The identity mapping (`plainMap()` in SVF) is the default and is
 * represented by an empty `ObjectClustering`.
 */
#pragma once

#include "IR/SVFG/SVFG.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace lotus::alias::flow_sensitive {

using ClusterObjectID = std::uint32_t;

class ObjectClustering {
public:
  bool empty() const { return representative_.empty(); }

  /// Representative of @p object; the object itself when unmerged.
  ClusterObjectID map(ClusterObjectID object) const {
    auto it = representative_.find(object);
    while (it != representative_.end()) {
      object = it->second;
      it = representative_.find(object);
    }
    return object;
  }

  /// Number of objects represented by @p object's class (1 when unmerged).
  std::size_t clusterSize(ClusterObjectID object) const {
    const auto it = sizes_.find(map(object));
    return it == sizes_.end() ? 1 : it->second;
  }

  /// Total number of merged (non-representative) objects.
  std::size_t mergedObjects() const { return merged_; }

  /// Number of equivalence classes with more than one member.
  std::size_t mergedClusters() const { return mergedClusters_; }

  /// Number of classes actually materialised (including singletons that were
  /// registered through `addSingleton`).
  std::size_t classes() const { return sizes_.size(); }

  /// Map every object in @p objects through the partition.
  template <typename Set> Set mapAll(const Set &objects) const {
    if (representative_.empty())
      return objects;
    Set mapped;
    for (ClusterObjectID object : objects)
      mapped.insert(map(object));
    return mapped;
  }

  /// Merge @p object's class into the class of @p representative.
  ///
  /// Both arguments may already belong to a class; the classes are folded and
  /// their sizes combined, so `merge()` can be called in any order.
  void merge(ClusterObjectID object, ClusterObjectID representative) {
    representative = map(representative);
    object = map(object);
    if (object == 0 || representative == 0 || object == representative)
      return;
    if (representative_.count(object) != 0)
      return;
    representative_[object] = representative;
    std::size_t objectSize = 1;
    if (const auto it = sizes_.find(object); it != sizes_.end()) {
      objectSize = it->second;
      sizes_.erase(it);
    }
    std::size_t &targetSize = sizes_[representative];
    if (targetSize == 0)
      targetSize = 1; // the representative itself
    const bool objectWasMerged = objectSize > 1;
    const bool targetWasMerged = targetSize > 1;
    targetSize += objectSize;
    // The object's class is replaced by the combined class: drop its cluster
    // count and add the target's only if it was a singleton before.
    if (objectWasMerged)
      --mergedClusters_;
    if (!targetWasMerged && targetSize > 1)
      ++mergedClusters_;
    ++merged_;
  }

  /// Register @p object as a singleton class so `clusterSize()` reports 1.
  void addSingleton(ClusterObjectID object) {
    if (object != 0 && sizes_.find(object) == sizes_.end())
      sizes_.emplace(object, 1);
  }

  const std::unordered_map<ClusterObjectID, ClusterObjectID> &mapping() const {
    return representative_;
  }

private:
  std::unordered_map<ClusterObjectID, ClusterObjectID> representative_;
  std::unordered_map<ClusterObjectID, std::size_t> sizes_;
  std::size_t merged_ = 0;
  std::size_t mergedClusters_ = 0;
};

/// Cluster objects that are indistinguishable by type and kind.
///
/// Only base objects are eligible: fields, field-insensitive representatives,
/// unknown/constant/function objects and arrays keep their identity, so the
/// partition never merges two different fields of one struct.  Objects must
/// have the same LLVM type and the same stack/heap/global/singleton flags.
inline ObjectClustering
clusterObjectsByType(const lotus::analysis::SVFG &graph) {
  struct Key {
    const llvm::Type *type = nullptr;
    bool stack = false;
    bool heap = false;
    bool global = false;
    bool singleton = false;
    bool operator==(const Key &other) const {
      return type == other.type && stack == other.stack && heap == other.heap &&
             global == other.global && singleton == other.singleton;
    }
  };
  struct KeyHash {
    std::size_t operator()(const Key &key) const {
      std::size_t hash = std::hash<const void *>{}(key.type);
      hash = hash * 31 + (key.stack ? 1 : 0);
      hash = hash * 31 + (key.heap ? 1 : 0);
      hash = hash * 31 + (key.global ? 1 : 0);
      hash = hash * 31 + (key.singleton ? 1 : 0);
      return hash;
    }
  };

  ObjectClustering clustering;
  std::unordered_map<Key, ClusterObjectID, KeyHash> owner;
  // Sort for determinism: the representative is always the smallest object id
  // of a class.
  std::vector<ClusterObjectID> objects = graph.getAllObjectIds();
  std::sort(objects.begin(), objects.end());
  for (ClusterObjectID object : objects) {
    const lotus::analysis::SVFG::ObjectInfo *info = graph.getObjectInfo(object);
    if (!info)
      continue;
    if (info->isUnknown || info->isConstant || info->isFunction ||
        info->isFieldInsensitive || info->isArray)
      continue;
    if (info->hasFieldOffset && info->fieldOffset != 0)
      continue;
    if (info->baseObjId != 0 && info->baseObjId != object)
      continue;
    const llvm::Value *allocation = graph.getObjectValue(object);
    if (!allocation)
      continue;
    const llvm::Type *type = allocation->getType();
    if (const auto *pointer = llvm::dyn_cast<llvm::PointerType>(type))
      type = pointer->getPointerElementType();
    const Key key{type, info->isStack, info->isHeap, info->isGlobal,
                  info->isSingleton};
    const auto it = owner.find(key);
    if (it == owner.end()) {
      owner.emplace(key, object);
      clustering.addSingleton(object);
    } else {
      clustering.merge(object, it->second);
    }
  }
  return clustering;
}

} // namespace lotus::alias::flow_sensitive
