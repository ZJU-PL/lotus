#pragma once

#include "Solvers/EGraph/EGraph.h"

#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <type_traits>
#include <unordered_set>

#include <llvm/ADT/SmallVector.h>

namespace lotus::egraph {

namespace detail {

template <typename...> using void_t = void;

template <typename CF, typename L, typename = void>
struct HasCostMethod : std::false_type {};

template <typename CF, typename L>
struct HasCostMethod<
    CF, L,
    void_t<decltype(std::declval<CF &>().cost(
        std::declval<const L &>(), std::function<typename CF::Cost(Id)>{}))>>
    : std::true_type {};

template <typename CF, typename L, typename = void>
struct HasCallOperator : std::false_type {};

template <typename CF, typename L>
struct HasCallOperator<
    CF, L,
    void_t<decltype(std::declval<CF &>()(
        std::declval<const L &>(), std::function<typename CF::Cost(Id)>{}))>>
    : std::true_type {};

template <typename CF, typename L, typename ChildCostFn>
auto invokeCost(CF &cost_fn, const L &node, ChildCostFn &&child_cost) ->
    typename CF::Cost {
  if constexpr (HasCostMethod<CF, L>::value) {
    return cost_fn.cost(node, std::forward<ChildCostFn>(child_cost));
  } else {
    static_assert(
        HasCallOperator<CF, L>::value,
        "Cost function must define Cost and either cost(node, child_cost) "
        "or operator()(node, child_cost)");
    return cost_fn(node, std::forward<ChildCostFn>(child_cost));
  }
}

template <typename T> T saturatingAdd(T lhs, T rhs) {
  static_assert(std::is_unsigned<T>::value,
                "saturatingAdd only supports unsigned integral types");
  constexpr T limit = std::numeric_limits<T>::max();
  return rhs > limit - lhs ? limit : static_cast<T>(lhs + rhs);
}

} // namespace detail

template <typename Derived, typename L, typename CostT> struct CostFunction {
  using Cost = CostT;

  Cost costRec(const RecExpr<L> &expr) {
    std::vector<Cost> costs;
    costs.reserve(expr.size());
    auto &derived = static_cast<Derived &>(*this);
    for (size_t i = 0; i < expr.size(); ++i) {
      Id id = Id::fromIndex(i);
      costs.push_back(derived.cost(
          expr[id], [&](Id child) { return costs.at(child.index()); }));
    }
    return costs.at(expr.root().index());
  }
};

template <typename L> struct AstSize : CostFunction<AstSize<L>, L, size_t> {
  using Cost = typename CostFunction<AstSize<L>, L, size_t>::Cost;

  template <typename C> Cost cost(const L &node, C &&child_cost) {
    size_t total = 1;
    for (Id child : node.children()) {
      total = detail::saturatingAdd(total, child_cost(child));
    }
    return total;
  }
};

template <typename L> struct AstDepth : CostFunction<AstDepth<L>, L, size_t> {
  using Cost = typename CostFunction<AstDepth<L>, L, size_t>::Cost;

  template <typename C> Cost cost(const L &node, C &&child_cost) {
    size_t max_child_depth = 0;
    for (Id child : node.children()) {
      max_child_depth = std::max(max_child_depth, child_cost(child));
    }
    return detail::saturatingAdd<size_t>(1, max_child_depth);
  }
};

template <typename L, typename A = NoAnalysis<L>, typename CostFn = AstSize<L>>
class Extractor {
public:
  using Cost = typename CostFn::Cost;

  Extractor(const EGraph<L, A> &egraph, CostFn cost_fn = CostFn())
      : egraph_(egraph), cost_fn_(std::move(cost_fn)) {
    compute();
  }

  std::pair<Cost, RecExpr<L>> findBest(Id eclass) const {
    Id canonical = egraph_.find(eclass);
    const auto &entry = bestEntry(canonical);
    return {entry.cost, buildExpr(canonical)};
  }

  const L &findBestNode(Id eclass) const {
    Id canonical = egraph_.find(eclass);
    uint32_t s = slot(canonical);
    if (best_node_cache_.size() < best_.size()) {
      best_node_cache_.resize(best_.size());
    }
    if (!best_node_cache_[s].has_value()) {
      best_node_cache_[s].emplace(
          egraph_.materializeEnode(bestEntry(canonical).enode_idx));
    }
    return *best_node_cache_[s];
  }

  Cost findBestCost(Id eclass) const {
    return bestEntry(egraph_.find(eclass)).cost;
  }

private:
  struct Entry {
    Cost cost{};
    uint32_t enode_idx = 0;
  };

  static constexpr uint32_t kNoSlot = UINT32_MAX;

  uint32_t slot(Id id) const noexcept {
    return id_to_slot_[egraph_.find(id).index()];
  }

  const Entry &bestEntry(Id id) const {
    return best_[slot(id)].value();
  }

  void compute() {
    const auto &active_ids = egraph_.activeClassIds();
    const size_t id_space = egraph_.idSpaceSize();
    id_to_slot_.assign(id_space, kNoSlot);
    const size_t nclasses = active_ids.size();
    for (uint32_t s = 0; s < nclasses; ++s) {
      id_to_slot_[active_ids[s].index()] = s;
    }

    best_.resize(nclasses);
    const bool is_clean = egraph_.clean();
    auto canon = [&](Id id) noexcept -> Id {
      return is_clean ? id : egraph_.find(id);
    };

    // Fast-path exact bucket-Dijkstra extraction for AstSize<L>:
    // - Zero L materialization on binary nodes!
    // - Zero O(d^2) class re-scanning (visits individual parent enodes)!
    // - Every class finalized at most ONCE in strictly increasing cost order!
    if constexpr (std::is_same_v<CostFn, AstSize<L>>) {
      constexpr size_t kInf = std::numeric_limits<size_t>::max();
      constexpr size_t kBuckets = 1024;
      std::vector<size_t> best_cost(nclasses, kInf);
      std::vector<uint32_t> best_nidx(nclasses, 0);
      std::vector<std::vector<uint32_t>> buckets(kBuckets);
      using HeapItem = std::pair<size_t, uint32_t>;
      std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>>
          overflow_heap;
      size_t min_bucket = kBuckets;
      size_t queued = 0;

      auto pushSlot = [&](size_t c, uint32_t s) {
        if (c < kBuckets) {
          buckets[c].push_back(s);
          if (c < min_bucket) {
            min_bucket = c;
          }
        } else {
          overflow_heap.emplace(c, s);
        }
        ++queued;
      };

      // 1. Seed ONLY arity-0 leaf nodes (cost == 1) in O(leaves)!
      egraph_.forEachLeafEnode([&](uint32_t nidx, Id owner, const L &) {
        uint32_t s = id_to_slot_[owner.index()];
        if (s != kNoSlot && 1 < best_cost[s]) {
          best_cost[s] = 1;
          best_nidx[s] = nidx;
          pushSlot(1, s);
        }
      });

      if (queued > 0) {
        egraph_.prepareParentEdgesForExtraction();
      }

      while (queued > 0) {
        size_t cur_cost = 0;
        uint32_t cur_slot = 0;
        while (min_bucket < kBuckets && buckets[min_bucket].empty()) {
          ++min_bucket;
        }
        if (min_bucket < kBuckets) {
          cur_cost = min_bucket;
          cur_slot = buckets[min_bucket].back();
          buckets[min_bucket].pop_back();
          --queued;
        } else {
          auto top = overflow_heap.top();
          overflow_heap.pop();
          --queued;
          cur_cost = top.first;
          cur_slot = top.second;
        }

        if (cur_cost != best_cost[cur_slot]) {
          continue; // Already finalized at a strictly smaller cost!
        }

        const Id cur_id = active_ids[cur_slot];
        egraph_.forEachParentEnode(cur_id, [&](uint32_t p_nidx, Id p_owner) {
          const uint32_t ps = id_to_slot_[p_owner.index()];
          if (ps == kNoSlot) {
            return;
          }
          size_t total = 1;
          if (egraph_.enodeIsBinary(p_nidx)) {
            auto [c0, c1] = egraph_.binaryEnodeChildren(p_nidx);
            const uint32_t s0 = id_to_slot_[canon(c0).index()];
            const uint32_t s1 = id_to_slot_[canon(c1).index()];
            if (s0 == kNoSlot || s1 == kNoSlot) {
              return;
            }
            const size_t k0 = best_cost[s0];
            const size_t k1 = best_cost[s1];
            if (k0 == kInf || k1 == kInf) {
              return;
            }
            total = (k0 > kInf - 1 - k1) ? kInf : (1 + k0 + k1);
          } else {
            bool ok = true;
            egraph_.forEachEnodeChild(p_nidx, [&](Id child) {
              if (!ok) {
                return;
              }
              const uint32_t cs = id_to_slot_[canon(child).index()];
              if (cs == kNoSlot || best_cost[cs] == kInf) {
                ok = false;
                return;
              }
              const size_t cc = best_cost[cs];
              total = (cc > kInf - total) ? kInf : (total + cc);
            });
            if (!ok) {
              return;
            }
          }
          if (total < best_cost[ps]) {
            best_cost[ps] = total;
            best_nidx[ps] = p_nidx;
            pushSlot(total, ps);
          }
        });
      }

      for (uint32_t s = 0; s < nclasses; ++s) {
        if (best_cost[s] != kInf) {
          best_[s] = Entry{best_cost[s], best_nidx[s]};
        }
      }
      return;
    }

    // General CostFn path: O(edges) per-parent-enode propagation (zero O(d^2)
    // class re-scanning!) with full soundness for non-monotone/cyclic costs!
    reach_marks_.assign(nclasses, 0);
    std::vector<uint8_t> in_queue(nclasses, 0);
    std::deque<Id> worklist;
    bool all_strictly_increasing = true;

    auto tryRelaxEnode = [&](Id owner, uint32_t owner_slot, uint32_t nidx,
                             const L &node) {
      const auto &children = node.children();
      for (Id child : children) {
        Id fc = canon(child);
        uint32_t cs = id_to_slot_[fc.index()];
        if (fc == owner || cs == kNoSlot || !best_[cs].has_value()) {
          return;
        }
      }

      Cost total_cost = detail::invokeCost(cost_fn_, node, [&](Id child) -> Cost {
        return best_[id_to_slot_[canon(child).index()]]->cost;
      });

      auto &old = best_[owner_slot];
      if (old && !(total_cost < old->cost)) {
        return;
      }

      bool strictly_inc = true;
      for (Id child : children) {
        const Cost &cc = best_[id_to_slot_[canon(child).index()]]->cost;
        if (!(cc < total_cost)) {
          strictly_inc = false;
          break;
        }
      }

      if (!(all_strictly_increasing && strictly_inc)) {
        if (wouldCreateCycle(owner, node)) {
          return;
        }
      }

      old = Entry{std::move(total_cost), nidx};
      if (!strictly_inc) {
        all_strictly_increasing = false;
      }
      if (!in_queue[owner_slot]) {
        in_queue[owner_slot] = 1;
        worklist.push_back(owner);
      }
    };

    // Seed with all leaf enodes (arity 0) in O(leaves)!
    egraph_.forEachLeafEnode([&](uint32_t nidx, Id owner, const L &node) {
      uint32_t s = id_to_slot_[owner.index()];
      if (s != kNoSlot) {
        tryRelaxEnode(owner, s, nidx, node);
      }
    });

    if (!worklist.empty()) {
      egraph_.prepareParentEdgesForExtraction();
    }

    while (!worklist.empty()) {
      Id id = worklist.front();
      worklist.pop_front();
      const uint32_t id_slot = id_to_slot_[id.index()];
      in_queue[id_slot] = 0;

      egraph_.forEachParentEnode(id, [&](uint32_t p_nidx, Id p_owner) {
        uint32_t ps = id_to_slot_[p_owner.index()];
        if (ps == kNoSlot) {
          return;
        }
        egraph_.visitEnode(p_nidx, [&](const L &node) {
          tryRelaxEnode(p_owner, ps, p_nidx, node);
        });
      });
    }
  }

  bool wouldCreateCycle(Id owner, const L &node) const {
    ++reach_generation_;
    if (reach_generation_ == 0) {
      std::fill(reach_marks_.begin(), reach_marks_.end(), 0);
      ++reach_generation_;
    }
    for (Id child : node.children()) {
      if (reaches(egraph_.find(child), owner)) {
        return true;
      }
    }
    return false;
  }

  bool reaches(Id current, Id target) const {
    if (current == target) {
      return true;
    }
    uint32_t current_slot = id_to_slot_[current.index()];
    if (reach_marks_[current_slot] == reach_generation_) {
      return false;
    }
    reach_marks_[current_slot] = reach_generation_;
    const auto &best = best_[current_slot];
    if (!best) {
      return false;
    }
    bool found = false;
    egraph_.forEachEnodeChild(best->enode_idx, [&](Id child) {
      if (!found && reaches(egraph_.find(child), target)) {
        found = true;
      }
    });
    return found;
  }

  RecExpr<L> buildExpr(Id eclass) const {
    RecExpr<L> expr;
    FlatHashMap<Id, Id> materialized;
    materialized.reserve(std::min<size_t>(best_.size(), 1024));
    FlatHashMap<L, Id, LanguageHash<L>> enodes;
    enodes.reserve(std::min<size_t>(best_.size(), 1024));
    std::vector<Id> stack{egraph_.find(eclass)};

    while (!stack.empty()) {
      Id current = egraph_.find(stack.back());
      if (materialized.count(current)) {
        stack.pop_back();
        continue;
      }

      L node = egraph_.materializeEnode(bestEntry(current).enode_idx);
      bool ready = true;
      for (Id child : node.children()) {
        Id canonical_child = egraph_.find(child);
        if (!materialized.count(canonical_child)) {
          stack.push_back(canonical_child);
          ready = false;
          break;
        }
      }
      if (!ready) {
        continue;
      }

      L rebuilt = node.mapChildren(
          [&](Id child) { return materialized.find(egraph_.find(child))->second; });
      auto found = enodes.find(rebuilt);
      Id output;
      if (found != enodes.end()) {
        output = found->second;
      } else {
        output = expr.add(rebuilt);
        enodes.try_emplace(std::move(rebuilt), output);
      }
      materialized.try_emplace(current, output);
      stack.pop_back();
    }
    return expr;
  }

  const EGraph<L, A> &egraph_;
  mutable CostFn cost_fn_;
  std::vector<uint32_t> id_to_slot_;
  std::vector<std::optional<Entry>> best_;
  mutable std::vector<std::optional<L>> best_node_cache_;
  mutable std::vector<size_t> reach_marks_;
  mutable size_t reach_generation_ = 0;
};

} // namespace lotus::egraph
