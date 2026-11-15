/**
 * @file AbstractStateStore.h
 * @brief Storage policy for the per-instruction abstract states kept by AE.
 *
 * Abstract execution in Lotus keys its state map by the LLVM instruction that
 * is being interpreted. The naive implementation keeps a *full* `AbstractState`
 * for every instruction it touches, which makes peak memory grow as
 * `#instructions x |state|`. Two observations make that unnecessary:
 *
 *  - Only these states are ever read again after the instruction that produced
 *    them: block heads (join points and cycle heads for widening), block
 *    terminators (a successor block's entry join), call sites (checkpoints,
 *    external API models and every detector query) and alloca sites (object
 *    size bookkeeping).
 *  - Instructions inside a block form a chain: `handleInstruction` copies the
 *    previous instruction's state and never looks further back.
 *
 * `StateLayout::Sparse` exploits both: a state created by intra-block
 * inheritance is *transient* and is dropped as soon as the next instruction has
 * inherited from it. States that a later reader can observe are *pinned* and
 * survive until the end of the analysis.
 *
 * The layout is a pure storage decision: both layouts answer exactly the same
 * queries, so switching layouts must not change a single finding. That is what
 * `AEStateLayoutEquivalenceTest` verifies.
 */

#pragma once

#include "Checker/AE/AbstractState.h"

#include <cstddef>
#include <map>
#include <unordered_set>

#include <llvm/IR/Instruction.h>

namespace lotus::analysis {

/// How `AbstractStateStore` retains abstract states.
enum class StateLayout {
  /// One full `AbstractState` per instruction. Matches the historical behavior
  /// and is the default. Every instruction's state stays queryable.
  Dense,
  /// SVF's semi-sparse layout: intermediate values (ValVars) live in one
  /// run-wide *def-site* table instead of being copied into every instruction's
  /// state, and states no later reader can observe are dropped. Object maps
  /// stay per instruction.
  SemiSparse,
  /// Same as `SemiSparse` in Lotus. SVF's `sparse` additionally builds an SVFG
  /// and stores object values at def sites too; Lotus's AE has no SVFG-backed
  /// value map, so the two levels coincide. Accepted so that SVF spellings keep
  /// working.
  Sparse,
};

/// Returns the stable command-line name of @p layout.
const char *toString(StateLayout layout);

/// Parses `dense` / `sparse` / `semi-sparse`. Returns false when unknown.
///
/// `semi-sparse` is accepted as an alias for `sparse`: AE stores the object map
/// and the value map in one state object, so there is no separate object-only
/// sparsity level to select. The alias exists so that option spellings carried
/// over from SVF (`--ae-sparsity=semi-sparse`) keep working.
bool parseStateLayout(llvm::StringRef name, StateLayout &layout);

/// Owns the abstract states of one AE run.
///
/// Nodes are `llvm::Instruction *`. Insertion is explicit (`set`, `inherit`,
/// `getOrCreate`) so the store can tell a pinned state from a transient one.
class AbstractStateStore {
public:
  using Node = const llvm::Instruction *;

  struct Statistics {
    std::size_t created = 0;  ///< States materialized.
    std::size_t released = 0; ///< Transient states dropped again.
    std::size_t peak = 0;     ///< Maximum number of live states.
  };

  explicit AbstractStateStore(StateLayout layout = StateLayout::Dense)
      : layout_(layout) {}

  StateLayout layout() const { return layout_; }

  /// True for both sparse levels: states are pruned and intermediate values
  /// live in the def-site table.
  bool isSparse() const { return layout_ != StateLayout::Dense; }

  /// True when intermediate values are stored once per definition.
  bool usesDefSiteVars() const { return isSparse(); }

  /// Switches layout. Only meaningful before the first state is created.
  void setLayout(StateLayout layout) { layout_ = layout; }

  /// The run-wide def-site value table (empty in `Dense`).
  AbstractState::VarToAbsValMap &defSiteVars() { return defSiteVars_; }
  const AbstractState::VarToAbsValMap &defSiteVars() const {
    return defSiteVars_;
  }

  /// True when a state exists for @p node.
  bool contains(Node node) const { return states_.count(node) != 0; }

  /// The state for @p node, or nullptr. Never inserts.
  AbstractState *find(Node node) {
    const auto it = states_.find(node);
    return it == states_.end() ? nullptr : &it->second;
  }

  /// The state for @p node, or nullptr. Never inserts.
  const AbstractState *find(Node node) const {
    const auto it = states_.find(node);
    return it == states_.end() ? nullptr : &it->second;
  }

  /// The state for @p node, inserting an empty one when absent.
  ///
  /// The node is *not* pinned: callers that expect the state to outlive the
  /// next `inherit` must call `pin` (or use `set`).
  AbstractState &getOrCreate(Node node) {
    const auto it = states_.find(node);
    if (it != states_.end())
      return it->second;
    ++stats_.created;
    AbstractState &state = states_[node];
    if (usesDefSiteVars())
      state.bindDefSiteVarTable(&defSiteVars_);
    stats_.peak = states_.size() > stats_.peak ? states_.size() : stats_.peak;
    return state;
  }

  /// Writes @p state to @p node and pins it: the state stays observable for the
  /// rest of the analysis (block heads, terminators, call sites, allocas).
  void set(Node node, const AbstractState &state) {
    AbstractState &slot = getOrCreate(node);
    writeInto(slot, state);
    pinned_.insert(node);
  }

  /// Writes @p state to @p node as a *transient* state, dropping the previous
  /// transient state of this block once it has been consumed.
  ///
  /// This is the intra-block look-behind: `handleInstruction` inherits from the
  /// instruction before it, so exactly one transient state has to stay alive.
  void inherit(Node node, const AbstractState &state) {
    // Copy *before* dropping the previous transient state: `state` may alias
    // exactly the state that is about to be released.
    AbstractState &slot = getOrCreate(node);
    writeInto(slot, state);

    if (!isSparse())
      return;

    if (lastTransient_ && lastTransient_ != node)
      release(lastTransient_);

    lastTransient_ = pinned_.count(node) == 0 ? node : nullptr;
  }

  /// Marks @p node as observable until `clear()`.
  void pin(Node node) { pinned_.insert(node); }

  bool isPinned(Node node) const { return pinned_.count(node) != 0; }

  /// Drops the state of @p node unless it is pinned.
  void release(Node node) {
    if (pinned_.count(node) != 0)
      return;
    if (lastTransient_ == node)
      lastTransient_ = nullptr;
    const auto it = states_.find(node);
    if (it == states_.end())
      return;
    states_.erase(it);
    ++stats_.released;
  }

  /// The state for @p node, inserting and pinning an empty one when absent.
  AbstractState &getOrCreatePinned(Node node) {
    AbstractState &state = getOrCreate(node);
    pinned_.insert(node);
    return state;
  }

  void clear() {
    states_.clear();
    pinned_.clear();
    defSiteVars_.clear();
    lastTransient_ = nullptr;
  }

  std::size_t size() const { return states_.size(); }
  std::size_t pinnedCount() const { return pinned_.size(); }
  const Statistics &stats() const { return stats_; }

private:
  /// Copies @p state into @p slot honouring the layout.
  ///
  /// In the def-site layout only the object map is per node: the variable
  /// entries are pushed into the run-wide table and the slot is rebound to it,
  /// so the values are stored exactly once.
  void writeInto(AbstractState &slot, const AbstractState &state) {
    if (!usesDefSiteVars()) {
      slot = state;
      return;
    }
    if (!state.usesDefSiteVarTable())
      state.varTable().scatterTo(defSiteVars_);
    slot = state;
    slot.bindDefSiteVarTable(&defSiteVars_);
  }

  StateLayout layout_;
  std::map<Node, AbstractState> states_;
  /// Run-wide home of the intermediate values in the def-site layout.
  AbstractState::VarToAbsValMap defSiteVars_;
  std::unordered_set<Node> pinned_;
  /// The single droppable state kept for intra-block inheritance.
  Node lastTransient_ = nullptr;
  Statistics stats_;
};

} // namespace lotus::analysis
