/// @file StrongUpdates.cpp
/// @brief Tuna-style staged strong updates for LotusAA load-store matching

#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"

#include <algorithm>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>

using namespace llvm;

void IntraLotusAA::ensureBBReachability() {
  if (bb_reachability_ready_)
    return;
  bb_reachability_ready_ = true;
  bb_to_scc_.clear();
  scc_reach_.clear();
  bb_avoiding_reachability_cache_.clear();

  if (!analyzed_func || analyzed_func->empty())
    return;

  // Iterative Tarjan SCC over ALL basic blocks (including unreachable blocks)
  // so SCCs are emitted in exact reverse topological order without risking
  // call-stack overflow on deep CFGs.
  std::vector<const BasicBlock *> blocks;
  blocks.reserve(analyzed_func->size());
  DenseMap<const BasicBlock *, unsigned> bb_idx;
  bb_idx.reserve(analyzed_func->size());
  for (const BasicBlock &BB : *analyzed_func) {
    unsigned id = static_cast<unsigned>(blocks.size());
    blocks.push_back(&BB);
    bb_idx.try_emplace(&BB, id);
  }

  const unsigned n = static_cast<unsigned>(blocks.size());
  std::vector<SmallVector<unsigned, 4>> adj(n);
  for (unsigned i = 0; i < n; ++i) {
    const BasicBlock *BB = blocks[i];
    if (BB->empty())
      continue;
    for (const BasicBlock *succ : successors(BB)) {
      if (!succ || succ->empty())
        continue;
      auto it = bb_idx.find(succ);
      if (it != bb_idx.end())
        adj[i].push_back(it->second);
    }
  }

  constexpr unsigned UNVISITED = std::numeric_limits<unsigned>::max();
  std::vector<unsigned> index(n, UNVISITED);
  std::vector<unsigned> lowlink(n, 0);
  std::vector<bool> on_stack(n, false);
  std::vector<unsigned> tarjan_stack;
  tarjan_stack.reserve(n);

  std::vector<unsigned> node_to_scc(n, UNVISITED);
  std::vector<SmallVector<unsigned, 4>> scc_members;

  struct Frame {
    unsigned node;
    unsigned edge_pos;
  };
  std::vector<Frame> call_stack;
  unsigned current_index = 0;

  for (unsigned root = 0; root < n; ++root) {
    if (index[root] != UNVISITED)
      continue;

    index[root] = lowlink[root] = current_index++;
    tarjan_stack.push_back(root);
    on_stack[root] = true;
    call_stack.push_back(Frame{root, 0});

    while (!call_stack.empty()) {
      Frame &frame = call_stack.back();
      unsigned u = frame.node;
      if (frame.edge_pos < adj[u].size()) {
        unsigned v = adj[u][frame.edge_pos++];
        if (index[v] == UNVISITED) {
          index[v] = lowlink[v] = current_index++;
          tarjan_stack.push_back(v);
          on_stack[v] = true;
          call_stack.push_back(Frame{v, 0});
        } else if (on_stack[v]) {
          lowlink[u] = std::min(lowlink[u], index[v]);
        }
      } else {
        call_stack.pop_back();
        if (!call_stack.empty()) {
          unsigned parent = call_stack.back().node;
          lowlink[parent] = std::min(lowlink[parent], lowlink[u]);
        }
        if (lowlink[u] == index[u]) {
          unsigned scc_id = static_cast<unsigned>(scc_members.size());
          scc_members.emplace_back();
          auto &members = scc_members.back();
          while (true) {
            unsigned w = tarjan_stack.back();
            tarjan_stack.pop_back();
            on_stack[w] = false;
            node_to_scc[w] = scc_id;
            members.push_back(w);
            if (w == u)
              break;
          }
        }
      }
    }
  }

  const unsigned num_sccs = static_cast<unsigned>(scc_members.size());
  bb_to_scc_.reserve(n);
  for (unsigned i = 0; i < n; ++i)
    bb_to_scc_.try_emplace(blocks[i], node_to_scc[i]);

  scc_reach_.assign(num_sccs, BitVector(num_sccs, false));
  // Tarjan emits SCCs in reverse topological order: every successor SCC has
  // strictly smaller ID than its predecessor SCC.
  for (unsigned scc_id = 0; scc_id < num_sccs; ++scc_id) {
    BitVector &reach = scc_reach_[scc_id];
    const auto &members = scc_members[scc_id];
    if (members.size() > 1)
      reach.set(scc_id);
    for (unsigned u : members) {
      for (unsigned v : adj[u]) {
        unsigned succ_scc = node_to_scc[v];
        if (succ_scc == scc_id) {
          reach.set(scc_id);
        } else if (!reach.test(succ_scc)) {
          reach.set(succ_scc);
          reach |= scc_reach_[succ_scc];
        }
      }
    }
  }
}

bool IntraLotusAA::bbReachesNonEmpty(const BasicBlock *src,
                                     const BasicBlock *dst) {
  ensureBBReachability();
  auto sit = bb_to_scc_.find(src);
  auto dit = bb_to_scc_.find(dst);
  if (sit == bb_to_scc_.end() || dit == bb_to_scc_.end())
    return false;
  return scc_reach_[sit->second].test(dit->second);
}

bool IntraLotusAA::instructionReaches(const Instruction *source,
                                      const Instruction *target,
                                      const Instruction *avoiding) {
  if (!source || !target || source == target ||
      source->getFunction() != target->getFunction()) {
    return false;
  }

  const BasicBlock *src_bb = source->getParent();
  const BasicBlock *dst_bb = target->getParent();
  if (!src_bb || !dst_bb)
    return false;

  if (!avoiding) {
    if (src_bb == dst_bb) {
      if (source->comesBefore(target))
        return true;
      return bbReachesNonEmpty(src_bb, src_bb);
    }
    return bbReachesNonEmpty(src_bb, dst_bb);
  }

  if (target == avoiding)
    return false;

  const BasicBlock *avoid_bb = avoiding->getParent();
  if (!avoid_bb)
    return instructionReaches(source, target, nullptr);

  // 1. Direct intra-block path from source to target without leaving src_bb.
  if (src_bb == dst_bb && source->comesBefore(target)) {
    if (!(avoid_bb == src_bb && source->comesBefore(avoiding) &&
          avoiding->comesBefore(target))) {
      return true;
    }
  }

  // 2. Any remaining path must exit src_bb; if avoiding sits strictly after
  // source in src_bb, exiting src_bb without hitting avoiding is impossible.
  if (avoid_bb == src_bb && source->comesBefore(avoiding))
    return false;

  // 3. After exiting src_bb, paths can only enter dst_bb at &dst_bb->front().
  if (dst_bb == avoid_bb) {
    if (!target->comesBefore(avoiding))
      return false;
    // The first entry into dst_bb reaches target before avoiding, so any
    // non-empty CFG path from src_bb to dst_bb suffices.
    return bbReachesNonEmpty(src_bb, dst_bb);
  }

  // 4. dst_bb != avoid_bb: need a non-empty CFG path from src_bb to dst_bb
  // that never visits avoid_bb after leaving src_bb.
  if (!bbReachesNonEmpty(src_bb, dst_bb))
    return false;
  if (src_bb == avoid_bb) {
    // Any path src_bb -> ... -> dst_bb has a simple suffix leaving src_bb via
    // one of its successors and reaching dst_bb without revisiting src_bb.
    return true;
  }
  if (!bbReachesNonEmpty(src_bb, avoid_bb) ||
      !bbReachesNonEmpty(avoid_bb, dst_bb)) {
    return true;
  }

  auto cache_key = std::make_tuple(src_bb, dst_bb, avoid_bb);
  auto cached = bb_avoiding_reachability_cache_.find(cache_key);
  if (cached != bb_avoiding_reachability_cache_.end())
    return cached->second;

  SmallVector<const BasicBlock *, 32> worklist;
  SmallPtrSet<const BasicBlock *, 32> visited;
  for (const BasicBlock *succ : successors(src_bb)) {
    if (!succ || succ->empty() || succ == avoid_bb)
      continue;
    if (succ == dst_bb) {
      bb_avoiding_reachability_cache_[cache_key] = true;
      return true;
    }
    if (bbReachesNonEmpty(succ, dst_bb) && visited.insert(succ).second)
      worklist.push_back(succ);
  }

  bool reaches = false;
  while (!worklist.empty()) {
    const BasicBlock *curr = worklist.pop_back_val();
    for (const BasicBlock *succ : successors(curr)) {
      if (!succ || succ->empty() || succ == avoid_bb)
        continue;
      if (succ == dst_bb) {
        reaches = true;
        break;
      }
      if (bbReachesNonEmpty(succ, dst_bb) && visited.insert(succ).second)
        worklist.push_back(succ);
    }
    if (reaches)
      break;
  }

  bb_avoiding_reachability_cache_[cache_key] = reaches;
  return reaches;
}

void IntraLotusAA::indexStoreForStrongUpdates(StoreInst *store,
                                              PTResult *store_pts) {
  if (!store || !store_pts)
    return;
  if (getSequenceNum(store) == VALUE_SEQ_UNDEF)
    return;
  const auto &sig = getGuardedPointsToSignature(store_pts, 0);
  for (const auto &entry : sig.entries) {
    locator_to_stores_[entry.first].push_back(store);
  }
}

void IntraLotusAA::indexLoadForStrongUpdates(LoadInst *load) {
  if (!load || !indexed_loads_.insert(load).second)
    return;
  PTResult *load_pts = findPTResult(load->getPointerOperand(), false);
  if (!load_pts)
    return;
  const auto &sig = getGuardedPointsToSignature(load_pts, 0);
  if (!sig.must_alias_eligible)
    return;
  fingerprint_to_loads_[sig.fingerprint].push_back(load);
  block_fp_loads_[std::make_pair(load->getParent(), sig.fingerprint)].push_back(
      load);
}

void IntraLotusAA::ensureAllLoadsIndexed() {
  if (all_loads_indexed_ || !analyzed_func)
    return;
  all_loads_indexed_ = true;
  for (BasicBlock &BB : *analyzed_func) {
    for (Instruction &I : BB) {
      if (auto *load = dyn_cast<LoadInst>(&I))
        indexLoadForStrongUpdates(load);
    }
  }
}

LoadInst *IntraLotusAA::findImmediateMustAliasAnchor(LoadInst *load) {
  if (!load || !dom_tree)
    return nullptr;

  // Ignore unreachable blocks. An unreachable block is dominated by
  // every block, which can cause non-termination.
  if (!dom_tree->isReachableFromEntry(load->getParent()))
    return nullptr;

  PTResult *load_pts = findPTResult(load->getPointerOperand(), false);
  if (!load_pts)
    return nullptr;
  const auto &load_sig = getGuardedPointsToSignature(load_pts, 0);
  if (!load_sig.must_alias_eligible)
    return nullptr;

  if (is_PTA_computed && !all_loads_indexed_)
    ensureAllLoadsIndexed();

  auto bucket_it = fingerprint_to_loads_.find(load_sig.fingerprint);
  if (bucket_it == fingerprint_to_loads_.end() || bucket_it->second.empty())
    return nullptr;

  const auto &bucket = bucket_it->second;
  if (bucket.size() <= 16) {
    LoadInst *best = nullptr;
    for (LoadInst *candidate : bucket) {
      if (!candidate || candidate == load ||
          !dom_tree->dominates(candidate, load) ||
          !areMustAliases(candidate->getPointerOperand(),
                          load->getPointerOperand())) {
        continue;
      }

      if (!best) {
        best = candidate;
        continue;
      }

      if (best->getParent() == candidate->getParent()) {
        if (best->comesBefore(candidate))
          best = candidate;
        continue;
      }
      if (dom_tree->dominates(best->getParent(), candidate->getParent()))
        best = candidate;
    }
    return best;
  }

  // Walk up the dominator tree starting from load's block; the latest matching
  // load in the closest dominating block is the exact immediate anchor.
  BasicBlock *load_bb = load->getParent();
  auto same_it =
      block_fp_loads_.find(std::make_pair(load_bb, load_sig.fingerprint));
  if (same_it != block_fp_loads_.end()) {
    LoadInst *best_same = nullptr;
    for (LoadInst *candidate : same_it->second) {
      if (!candidate || candidate == load || !candidate->comesBefore(load) ||
          !areMustAliases(candidate->getPointerOperand(),
                          load->getPointerOperand())) {
        continue;
      }
      if (!best_same || best_same->comesBefore(candidate))
        best_same = candidate;
    }
    if (best_same)
      return best_same;
  }

  DomTreeNode *node = dom_tree->getNode(load_bb);
  if (!node)
    return nullptr;
  for (node = node->getIDom(); node; node = node->getIDom()) {
    BasicBlock *anc_bb = node->getBlock();
    auto anc_it =
        block_fp_loads_.find(std::make_pair(anc_bb, load_sig.fingerprint));
    if (anc_it == block_fp_loads_.end())
      continue;
    LoadInst *best_anc = nullptr;
    for (LoadInst *candidate : anc_it->second) {
      if (!candidate || !areMustAliases(candidate->getPointerOperand(),
                                        load->getPointerOperand())) {
        continue;
      }
      if (!best_anc || best_anc->comesBefore(candidate))
        best_anc = candidate;
    }
    if (best_anc)
      return best_anc;
  }

  return nullptr;
}

bool IntraLotusAA::mustKill(StoreInst *killer, StoreInst *killed,
                            LoadInst *load) {
  if (!killer || !killed || !load || killer == killed)
    return false;
  if (!areMustAliases(killer->getPointerOperand(),
                      killed->getPointerOperand())) {
    return false;
  }
  if (!instructionReaches(killed, killer) ||
      !instructionReaches(killer, load)) {
    return false;
  }

  // Standard dominance discharges the common case in constant time.  The
  // reachability-with-removal fallback also catches the paper's more general
  // relation where paths that never execute the killed store may bypass the
  // killer on their way to the load.
  if (dom_tree && dom_tree->dominates(killer, load))
    return true;

  // Definition 3.1: every killed-to-load path must pass through the killer.
  // Testing reachability with the killer removed handles same-block ordering,
  // branches, and joins uniformly without requiring a full post-dominator
  // tree rooted at each load.
  return !instructionReaches(killed, load, killer);
}

IntraLotusAA::MustKillForest &
IntraLotusAA::getOrCreateMustKillForest(LoadInst *load) {
  auto cached = must_kill_forests.find(load);
  if (cached != must_kill_forests.end())
    return cached->second;

  MustKillForest forest;
  forest.anchor = findImmediateMustAliasAnchor(load);
  if (forest.anchor) {
    MustKillForest &anchor_forest = getOrCreateMustKillForest(forest.anchor);
    forest.stores = anchor_forest.stores;
    forest.roots = anchor_forest.roots;
  }

  const int load_sequence = getSequenceNum(load);
  const int anchor_sequence =
      forest.anchor ? getSequenceNum(forest.anchor) : VALUE_SEQ_UNDEF;

  if (load_sequence != VALUE_SEQ_UNDEF) {
    PTResult *load_pts = findPTResult(load->getPointerOperand(), false);
    if (load_pts) {
      const auto &load_sig = getGuardedPointsToSignature(load_pts, 0);
      SmallPtrSet<StoreInst *, 16> seen_stores;
      const bool need_dedup = load_sig.entries.size() > 1;

      for (const auto &entry : load_sig.entries) {
        auto sit = locator_to_stores_.find(entry.first);
        if (sit == locator_to_stores_.end())
          continue;

        const auto &stores = sit->second;
        const auto *it = stores.begin();
        if (forest.anchor) {
          it = std::upper_bound(
              stores.begin(), stores.end(), anchor_sequence,
              [this](int seq, StoreInst *st) {
                return seq < getSequenceNum(st);
              });
        }

        for (; it != stores.end(); ++it) {
          StoreInst *store = *it;
          const int store_sequence = getSequenceNum(store);
          if (store_sequence >= load_sequence)
            break;
          if (store_sequence == VALUE_SEQ_UNDEF)
            continue;
          if (need_dedup && !seen_stores.insert(store).second)
            continue;
          if (forest.anchor && !instructionReaches(forest.anchor, store))
            continue;
          if (!instructionReaches(store, load))
            continue;

          if (!mayAlias(load->getPointerOperand(), store->getPointerOperand()))
            continue;

          forest.stores.insert(store);
          forest.roots.insert(store);
        }
      }
    }
  }

  // Partition surviving roots by must-alias fingerprint before pairwise
  // mustKill() checks, since mustKill requires areMustAliases(killer, killed).
  if (forest.roots.size() > 1) {
    DenseMap<size_t, SmallVector<StoreInst *, 4>> fp_groups;
    for (Instruction *root_inst : forest.roots) {
      auto *store = cast<StoreInst>(root_inst);
      PTResult *store_pts = findPTResult(store->getPointerOperand(), false);
      if (!store_pts)
        continue;
      const auto &sig = getGuardedPointsToSignature(store_pts, 0);
      if (!sig.must_alias_eligible)
        continue;
      fp_groups[sig.fingerprint].push_back(store);
    }

    SmallPtrSet<Instruction *, 16> killed_roots;
    for (auto &group_entry : fp_groups) {
      const auto &group = group_entry.second;
      if (group.size() <= 1)
        continue;
      for (StoreInst *killer : group) {
        for (StoreInst *killed : group) {
          if (killer == killed || killed_roots.count(killed))
            continue;
          if (mustKill(killer, killed, load))
            killed_roots.insert(killed);
        }
      }
    }
    for (Instruction *killed : killed_roots)
      forest.roots.erase(killed);
  }

  return must_kill_forests.emplace(load, std::move(forest)).first->second;
}

void IntraLotusAA::collectPathSensitiveLoadValues(LoadInst *load,
                                                  mem_value_t &result,
                                                  bool create_symbol) {
  result.clear();
  if (!load)
    return;

  const std::set<Instruction *, llvm_cmp> *surviving_stores = nullptr;
  if (IntraLotusAAConfig::lotus_enable_must_kill)
    surviving_stores = &getOrCreateMustKillForest(load).roots;

  if (create_symbol) {
    loadPtrAt(load->getPointerOperand(), load, result, true, 0,
              ObjectLocator::FUNC_LEVEL_UNDEFINED, nullptr, false, true,
              surviving_stores);
  } else {
    loadPtrAt(load->getPointerOperand(), load, result, false, 0, 0,
              func_obj ? func_obj->findLocator(0, false) : nullptr, true, true,
              surviving_stores);
  }
  // The GVFG adapter historically requested a refined producer set, whereas
  // transfer functions let PTResultIterator perform the final merge. Preserve
  // that distinction while sharing the must-kill filtering stage.
  if (!create_symbol)
    refineResult(result);
}

IntraLotusAA::StrongUpdateStats
IntraLotusAA::getStrongUpdateStats(LoadInst *load) {
  StrongUpdateStats stats;
  if (!load)
    return stats;
  MustKillForest &forest = getOrCreateMustKillForest(load);
  stats.anchor = forest.anchor;
  stats.candidate_store_count = static_cast<unsigned>(forest.stores.size());
  stats.root_store_count = static_cast<unsigned>(forest.roots.size());
  return stats;
}
