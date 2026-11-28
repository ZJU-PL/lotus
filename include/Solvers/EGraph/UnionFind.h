#pragma once

#include "Solvers/EGraph/FlatHashMap.h"
#include "Solvers/EGraph/Id.h"

namespace lotus::egraph {

class UnionFind {
public:
  Id makeSet() {
    Id id = Id::fromIndex(parents_.size());
    parents_.push_back(id);
    return id;
  }

  inline Id makeSetUnchecked() noexcept {
    Id id = Id::fromIndex(parents_.size());
    parents_.pushBackUnchecked(id);
    return id;
  }

  size_t size() const noexcept { return parents_.size(); }
  const Id *parentsData() const noexcept { return parents_.data(); }
  void reserve(size_t n) { parents_.reserve(n); }
  void reserveAdditional(size_t extra) { parents_.reserveAdditional(extra); }
  bool isRoot(Id id) const noexcept { return parents_[id.index()] == id; }
  void prefetch(Id id) const noexcept {
    __builtin_prefetch(&parents_[id.index()], 0, 1);
  }

  Id find(Id current) const noexcept {
    Id p = parents_[current.index()];
    if (current == p) {
      return current;
    }
    while (current != p) {
      Id grandparent = parents_[p.index()];
      parents_[current.index()] = grandparent;
      current = grandparent;
      p = parents_[current.index()];
    }
    return current;
  }

  Id findMut(Id current) noexcept { return find(current); }

  Id unite(Id root1, Id root2) noexcept {
    parents_[root2.index()] = root1;
    return root1;
  }

private:
  mutable PodVec<Id> parents_;
};

} // namespace lotus::egraph
