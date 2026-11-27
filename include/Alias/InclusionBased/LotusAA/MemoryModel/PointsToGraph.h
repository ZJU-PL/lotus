/*
 * LotusAA - Points-To Graph Base Class
 * 
 * Base class for pointer analysis results. Provides common infrastructure
 * for managing points-to information, memory objects, and constraints.
 * 
 * Key Concepts:
 * - PTResult: Maps pointers to sets of (MemObject, offset) pairs
 * - MemObject: Abstract representation of memory locations
 * - Field-sensitive: Tracks individual struct fields separately
 */

/// @file PointsToGraph.h
/// @brief Points-to graph base class — per-function graph of (pointer → target
///        set) relationships
///
/// `PTGraph` is the per-function container for points-to information.  It
/// holds:
///   - `pt_results` — map from LLVM `Value *` to `PTResult *` (points-to sets)
///   - `mem_objs` — the set of `MemObject`s allocated in the function
///   - `global_objects` — objects corresponding to global variables
///   - `NullPTS` — a singleton null-pointer points-to set
///   - load-load matching caches (`load_category`, `load_category_collection`)
///   - object-to-call access-path depth caches
///   - path-condition caches (unit regions, AND/OR/NOT regions, phi regions,
///     block regions, semantic regions, call-target conditions)
///   - dominator tree for SSA-style locator versioning
///   - controlled-dependence info for path-condition construction
///
/// Subclasses (`IntraLotusAA`) implement the per-function analysis driver and
/// transfer functions.
///
/// ## PTResult
///
/// A points-to set for a single pointer.  Targets are either:
///   - **Direct**: `(ObjectLocator *)` — the pointer points to that locator
///   - **Derived**: `(PTResult *src, int64_t offset)` — inherits src's targets
///     plus an offset
///
/// Both carry a `path_cond_t` guard and are flattened on iteration via
/// `PTResultIterator`.

#pragma once

#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include "Alias/InclusionBased/LotusAA/MemoryModel/MemObject.h"
#include "Alias/InclusionBased/LotusAA/MemoryModel/Types.h"
#include "Alias/InclusionBased/LotusAA/Support/Compat.h"

namespace llvm {

// Forward declarations
class LotusAA;
class PTResultIterator;
class PTGraph;

/*
 * PTResult - Points-to set for a pointer (simplified)
 * 
 * Two types of targets:
 * 1. Direct: <ObjectLocator> - ptr points to locator
 * 2. Derived: <PTResult', offset> - ptr points to (PTResult' + offset)
 */
/// Points-to set for a single pointer value.
///
/// Two target forms:
///   1. **Direct**:   `<path_cond_t, ObjectLocator>` — ptr → loc under cond
///   2. **Derived**:  `<path_cond_t, PTResult*, int64_t offset>` — ptr inherits
///      the targets of `src_pts` at additional `offset` under cond
///
/// `PTResultIterator` flattens both forms into a `map<ObjectLocator*, cond>`.
class PTResult {
public:
  // Direct points-to target
  struct PtItem {
    path_cond_t cond;
    ObjectLocator *locator;
    PtItem(path_cond_t cond, ObjectLocator *loc) : cond(cond), locator(loc) {}
    PtItem(const PtItem &item) : cond(item.cond), locator(item.locator) {}
  };

  // Derived points-to target  
  struct DerivedPtItem {
    path_cond_t cond;
    PTResult *src_pts;
    int64_t offset;

    DerivedPtItem(path_cond_t cond, PTResult *other_pts, int64_t off)
        : cond(cond), src_pts(other_pts), offset(off) {}
    DerivedPtItem(const DerivedPtItem &itemset)
        : cond(itemset.cond), src_pts(itemset.src_pts),
          offset(itemset.offset) {}
  };

private:
  std::vector<PtItem> pt_list;
  std::vector<DerivedPtItem> derived_list;
  Value *ptr;
  bool is_optimized;
  unsigned revision;

  friend class PTResultIterator;

public:
  PTResult(Value *ptr) : ptr(ptr), is_optimized(false), revision(0) {}

  Value *get_ptr() { return ptr; }
  unsigned getRevision() const { return revision; }

  void add_target(path_cond_t cond, MemObject *obj, int64_t offset) {
    ObjectLocator *locator = obj->findLocator(offset, true);
    pt_list.push_back({cond, locator});
    is_optimized = false;
    ++revision;
  }

  void add_derived_target(path_cond_t cond, PTResult *src_pts, int64_t offset) {
    derived_list.push_back({cond, src_pts, offset});
    is_optimized = false;
    ++revision;
  }
};

/*
 * PTResultIterator - Collect final points-to results
 */
/// Iterator that flattens a `PTResult` (including derived targets) into a
/// map from `ObjectLocator*` to `path_cond_t`.  Recursively visits the
/// derived-target graph with cycle detection (a `visited` set).  On first
/// construction caches the flattened result in the PTResult for O(1) reuse.
class PTResultIterator {
public:
  using iterator = std::map<ObjectLocator *, path_cond_t, obj_loc_cmp>::iterator;
  using size_type = std::map<ObjectLocator *, path_cond_t, obj_loc_cmp>::size_type;

private:
  std::map<ObjectLocator *, path_cond_t, obj_loc_cmp> res;
  PTGraph *parent_graph;

  void visit(PTResult *target, int64_t off, path_cond_t cond,
             std::set<PTResult *> &visited,
             std::map<std::pair<PTResult *, int64_t>, path_cond_t> &seen_cond);

public:
  PTResultIterator(PTResult *target, PTGraph *parent_graph);

  iterator begin() { return res.begin(); }
  iterator end() { return res.end(); }
  size_type count(ObjectLocator *loc) { return res.count(loc); }
  path_cond_t get(ObjectLocator *loc) {
    assert(res.count(loc) && "The result does not contain the queried locator");
    return res[loc];
  }
  int size() { return res.size(); }

  friend raw_ostream &operator<<(raw_ostream &out, PTResultIterator &pt_it);
};

/// Hybrid complement-edge ROBDD + hash-consed AIG + lazy incremental Tseitin
/// SAT manager for exact PathCond Boolean reasoning.
///
/// On typical functions the ROBDD stays compact and answers equivalence,
/// tautology, and satisfiability in O(1). When a high-cutwidth CFG (e.g., a
/// large goto/switch state machine) exceeds the per-op or per-function BDD
/// budget, construction stays O(1) via the exact complement-edge AIG and
/// satisfiability/tautology queries are answered exactly by incremental
/// propositional CDCL SAT without any loss of soundness or precision.
class PathCondBddManager {
public:
  static constexpr uint32_t INVALID_ID = PathCond::INVALID_BDD_ID;
  static constexpr uint32_t FALSE_ID = PathCond::FALSE_BDD_ID;
  static constexpr uint32_t TRUE_ID = PathCond::TRUE_BDD_ID;
  static constexpr size_t MAX_BDD_NODES = 65536;
  static constexpr unsigned MAX_BDD_STEPS = 4096;

  struct FormulaIds {
    uint32_t bdd;
    uint32_t aig;
  };

private:
  struct Node {
    uint32_t var;
    uint32_t low;  // Always regular (even)
    uint32_t high; // Regular or complemented
  };

  struct AigNode {
    uint32_t lhs; // For primary input vars: lhs == rhs == var
    uint32_t rhs; // For AND gates: lhs < rhs
    // 256-bit concrete simulation signatures across independent assignments
    // (bit 0 = all-0, bit 1 = all-1, remaining bits deterministic pseudorandom).
    // Since all primary input variables are unconstrained, any non-zero bit
    // witnesses exact satisfiability in O(1) without invoking SAT.
    uint64_t sim[4];
  };

  struct NodeKey {
    uint32_t var;
    uint32_t low;
    uint32_t high;

    bool operator==(const NodeKey &other) const {
      return var == other.var && low == other.low && high == other.high;
    }
  };

  struct NodeKeyInfo {
    static inline NodeKey getEmptyKey() {
      return NodeKey{std::numeric_limits<uint32_t>::max(), 0, 0};
    }
    static inline NodeKey getTombstoneKey() {
      return NodeKey{std::numeric_limits<uint32_t>::max() - 1, 0, 0};
    }
    static unsigned getHashValue(const NodeKey &key) {
      return static_cast<unsigned>(hash_combine(key.var, key.low, key.high));
    }
    static bool isEqual(const NodeKey &lhs, const NodeKey &rhs) {
      return lhs == rhs;
    }
  };

  struct SwitchEncoding {
    FormulaIds default_ids{FALSE_ID, FALSE_ID};
    DenseMap<const ConstantInt *, FormulaIds> case_ids;
  };

  struct MutexGroup {
    FormulaIds none_yet{TRUE_ID, TRUE_ID};
    std::map<const void *, FormulaIds> member_ids;
  };

  struct BranchNoValEntry {
    uint32_t var = std::numeric_limits<uint32_t>::max();
    const BasicBlock *first_succ = nullptr;
  };

  struct SatState;

  std::vector<Node> nodes_;
  std::vector<AigNode> aig_nodes_;
  std::vector<uint32_t> var_to_aig_;
  uint32_t next_var_ = 0;
  DenseMap<NodeKey, uint32_t, NodeKeyInfo> unique_table_;
  DenseMap<uint64_t, uint32_t> and_cache_;
  DenseMap<uint64_t, uint32_t> aig_unique_;

  DenseMap<const Value *, uint32_t> value_vars_;
  uint32_t null_value_var_ = std::numeric_limits<uint32_t>::max();
  DenseMap<const BasicBlock *, BranchNoValEntry> branch_noval_vars_;
  DenseMap<const BasicBlock *, uint32_t> invoke_vars_;
  DenseMap<const BasicBlock *, uint32_t> block_vars_;
  DenseMap<const PathCond *, uint32_t> imported_vars_;
  std::map<std::pair<const BasicBlock *, const Value *>, SwitchEncoding>
      switch_encodings_;
  std::map<std::pair<const BasicBlock *, const Value *>, MutexGroup>
      fallback_switch_groups_;
  DenseMap<const Value *, MutexGroup> call_target_groups_;
  MutexGroup null_call_target_group_;
  DenseMap<const BasicBlock *, MutexGroup> fallback_branch_groups_;

  mutable std::mutex sat_mutex_;
  mutable std::unique_ptr<SatState> sat_state_;
  mutable DenseMap<uint32_t, uint8_t> aig_sat_cache_;

  uint32_t allocVar();
  uint32_t mkBounded(uint32_t var, uint32_t low, uint32_t high);
  uint32_t bddAndBounded(uint32_t u, uint32_t v, unsigned &steps);
  uint32_t tryBddAnd(uint32_t u, uint32_t v);
  bool isAigGate(uint32_t id) const {
    return id > 1U && aig_nodes_[id >> 1].lhs != aig_nodes_[id >> 1].rhs;
  }
  uint32_t aigVar(uint32_t var) const { return var_to_aig_[var]; }
  uint32_t aigAnd(uint32_t a, uint32_t b);
  uint32_t aigOr(uint32_t a, uint32_t b) {
    return aigNot(aigAnd(aigNot(a), aigNot(b)));
  }
  FormulaIds makeVarIds(uint32_t var);
  FormulaIds encodeMutexMember(MutexGroup &group, const void *member_key);
  uint32_t getValueVar(const Value *value);

public:
  PathCondBddManager();
  ~PathCondBddManager();
  PathCondBddManager(const PathCondBddManager &) = delete;
  PathCondBddManager &operator=(const PathCondBddManager &) = delete;

  static uint32_t bddNot(uint32_t id) { return id ^ 1U; }
  static uint32_t aigNot(uint32_t id) { return id ^ 1U; }

  FormulaIds combineNot(FormulaIds x) const {
    return FormulaIds{x.bdd == INVALID_ID ? INVALID_ID : bddNot(x.bdd),
                      aigNot(x.aig)};
  }
  FormulaIds combineAnd(FormulaIds lhs, FormulaIds rhs);
  FormulaIds combineOr(FormulaIds lhs, FormulaIds rhs) {
    return combineNot(combineAnd(combineNot(lhs), combineNot(rhs)));
  }

  FormulaIds encodeValueAtom(const Value *value, bool sense);
  FormulaIds encodeBranchAtom(const BasicBlock *block,
                              const BasicBlock *successor, const Value *value,
                              bool sense);
  FormulaIds encodeSwitchAtom(const BasicBlock *block, const Value *value,
                              const ConstantInt *case_value, bool is_default);
  FormulaIds encodeInvokeAtom(const BasicBlock *block, bool is_normal);
  FormulaIds encodeBlockAtom(const BasicBlock *block);
  FormulaIds encodeCallTargetAtom(const Value *value, const Function *callee);
  FormulaIds encodeImportedAtom(const PathCond *imported);

  int getCachedAigSat(uint32_t aig_id) const;
  bool isAigSatisfiable(uint32_t aig_id) const;
};

/*
 * PTGraph - Points-to graph manager (simplified)
 */
/// Per-function points-to graph manager.
///
/// Owns all `PTResult`, `MemObject`, and path-condition data for one function.
/// Provides the core operations needed by instruction transfer functions:
///   - `addPointsTo(ptr, obj, offset, cond)` — create a direct points-to edge
///   - `derivePtsFrom(ptr, src_pts, offset, cond)` — create a derived edge
///   - `assignPts(ptr, pts)` — assign a shared set (e.g. NullPTS)
///   - `loadPtrAt(ptr, from_loc, result)` — load values from memory locations
///   - `trackPtrRightValue(val, result)` — track through PHI/Select/Load
///   - `refineResult(to_refine)` — merge duplicate values with OR conditions
///
/// Path-condition queries (`getEmptyCond`, `getValueCond`, `getBlockCond`,
/// `findOrCreateUnitRegion`, `findOrCreateAndRegion`, etc.) are used by
/// transfer functions to attach guards to points-to targets.
class PTGraph {
public:
  enum PTGType { PTGBegin, PTGraphTy, IntraLotusAATy, PTGEnd };

  virtual PTGType getKind() const { return PTGraphTy; }

  static bool classof(const PTGraph *G) {
    return G->getKind() >= PTGBegin && G->getKind() <= PTGEnd;
  }

protected:
  // Parent function being analyzed
  Function *analyzed_func;

  // Parent LotusAA pass
  LotusAA *lotus_aa;

  // Dominance information for SSA construction
  DominatorTree *dom_tree;
  PostDominatorTree *post_dom_tree;

  // Special NULL result
  PTResult *NullPTS;

  // Points-to results
  std::map<Value *, PTResult *, llvm_cmp> pt_results;

  // Memory objects
  std::map<MemObject *, int, mem_obj_cmp> mem_objs;

  int pt_index;
  int obj_index;

  // Load-load matching
  std::map<LoadInst *, int, llvm_cmp> load_category;
  std::vector<std::set<LoadInst *, llvm_cmp> *> load_category_collection;
  bool load_load_match_performed;

  // Global objects
  std::set<MemObject *, mem_obj_cmp> global_objects;

  // Object-to-call access-path depth caches
  std::map<Value *, std::map<MemObject *, int, mem_obj_cmp>, llvm_cmp>
      object_call_arg_ap_depth_cache;
  std::map<Value *, std::set<MemObject *, mem_obj_cmp>, llvm_cmp>
      object_call_ap_depth_frontier;

  DenseMap<std::pair<BasicBlock *, BasicBlock *>, path_cond_t>
      phi_region_cache;
  DenseMap<std::pair<BasicBlock *, BasicBlock *>, path_cond_t> bb_region_cache;
  DenseSet<std::pair<BasicBlock *, BasicBlock *>> bb_region_active_;
  DenseMap<BasicBlock *, path_cond_t> unit_region_cache_;
  SmallPtrSet<BasicBlock *, 16> visited;
  DenseMap<BasicBlock *, DenseMap<BasicBlock *, path_cond_t>>
      control_dep_cache_;
  DenseMap<std::pair<BasicBlock *, BasicBlock *>, path_cond_t> edge_cond_cache_;

  path_cond_t true_cond_;
  path_cond_t false_cond_;
  std::vector<std::unique_ptr<PathCond>> cond_nodes_;
  PathCondBddManager bdd_mgr_;
  DenseMap<uint32_t, path_cond_t> formula_cond_cache_;
  DenseMap<uint32_t, path_cond_t> aig_cond_cache_;
  DenseMap<const PathCond *, PathCondBddManager::FormulaIds>
      foreign_formula_cache_;
  DenseMap<std::pair<Value *, int>, path_cond_t> value_cond_cache_;
  DenseMap<std::pair<Value *, Function *>, path_cond_t> call_target_cond_cache_;
  DenseMap<path_cond_t, path_cond_t> imported_cond_cache_;
  DenseMap<path_cond_t, path_cond_t> not_cond_cache_;
  DenseMap<std::pair<path_cond_t, path_cond_t>, path_cond_t> and_cond_cache_;
  DenseMap<std::pair<path_cond_t, path_cond_t>, path_cond_t> or_cond_cache_;
  DenseMap<BasicBlock *, unsigned> bb_rpo_order_;
  DenseMap<BasicBlock *, SmallVector<BasicBlock *, 8>> idf_cache_;
  bool control_dep_ready_;

  struct GuardedPointsToSignature {
    unsigned revision = 0;
    size_t fingerprint = 0;
    bool must_alias_eligible = false;
    std::vector<std::pair<ObjectLocator *, path_cond_t>> entries;
  };
  std::map<std::pair<PTResult *, int64_t>, GuardedPointsToSignature>
      guarded_points_to_signature_cache_;
  struct MayAliasCacheEntry {
    unsigned rev1 = 0;
    unsigned rev2 = 0;
    bool result = false;
  };
  std::map<std::tuple<PTResult *, int64_t, PTResult *, int64_t>,
           MayAliasCacheEntry>
      may_alias_cache_;

  // Constants
  static const int VALUE_SEQ_UNDEF;
  static const int VALUE_SEQ_INFINITE;
  static const int FUNC_OBJ_UNREACHABLE;

protected:
  Type *normalizeType(Type *type);
  MemObject *newObject(Value *alloc_site,
                       MemObject::ObjKind obj_type = MemObject::CONCRETE);

  PTResult *addPointsTo(Value *ptr, MemObject *obj, int64_t offset,
                        path_cond_t cond);
  PTResult *derivePtsFrom(Value *ptr, PTResult *other_pts, int64_t offset,
                          path_cond_t cond);
  PTResult *assignPts(Value *ptr, PTResult *pts);

  void refineResult(mem_value_t &to_refine);

  void loadPtrAt(Value *ptr, Instruction *from_loc, mem_value_t &res,
                 bool create_symbol = false, int64_t offset = 0,
                 int func_level = ObjectLocator::FUNC_LEVEL_UNDEFINED,
                 ObjectLocator *func_call_cache = nullptr,
                 bool is_include_func_summary = false,
                 bool is_maintain_load_map = true,
                 const std::set<Instruction *, llvm_cmp>
                     *surviving_store_positions = nullptr);
  void loadPtrAtImpl(Value *ptr, Instruction *from_loc, mem_value_t &result,
                     bool create_symbol, int64_t query_offset, int func_level,
                     ObjectLocator *func_call_cache,
                     bool is_include_func_summary, bool is_maintain_load_map,
                     std::set<std::tuple<Value *, Instruction *, int64_t>>
                         &visited,
                     const std::set<Instruction *, llvm_cmp>
                         *surviving_store_positions);

  void trackPtrRightValue(Value *ptr, mem_value_t &res);
  void trackPtrRightValueUnderCondition(Value *ptr, mem_value_t &res,
                                        path_cond_t base_cond,
                                        float base_confidence);

  void performLoadLoadMatch();
  bool cacheLoadCategory(LoadInst *load_inst);
  void buildControlDependenceInfo();
  path_cond_t getCFGEdgeCond(BasicBlock *src_bb, BasicBlock *succ_bb);
  path_cond_t localizePathCond(path_cond_t cond);
  path_cond_t getComplementaryBranchCond(path_cond_t cond);
  PathCondBddManager::FormulaIds getFormulaIds(path_cond_t cond);
  PathCondBddManager::FormulaIds computeFormulaIds(const PathCond *cond);
  path_cond_t lookupCachedCond(PathCondBddManager::FormulaIds ids) const;
  path_cond_t internCondWithIds(PathCondBddManager::FormulaIds ids,
                                std::unique_ptr<PathCond> cond);
  path_cond_t internCond(std::unique_ptr<PathCond> cond);
  bool isKnownTrue(path_cond_t cond) const;
  bool isKnownFalse(path_cond_t cond) const;
  const SmallVector<BasicBlock *, 8> &
  getIteratedDominanceFrontier(BasicBlock *bb);
  path_cond_t importPathCond(path_cond_t cond, Value *callsite,
                             Function *callee);
  const GuardedPointsToSignature &
  getGuardedPointsToSignature(PTResult *points_to, int64_t offset);

  virtual int getSequenceNum(Value *val) = 0;
  virtual int getInlineApDepth() = 0;
  virtual PTGraph *getPtGraph(Function *F) = 0;

public:
  PTGraph(Function *F, LotusAA *lotus_aa);
  virtual ~PTGraph();

  int getPtIndex() { return pt_index; }

  void getLoadValues(Value *ptr, Instruction *from_loc, mem_value_t &res,
                     int64_t offset = 0);

  bool isSameValue(Value *ptr1, Instruction *loc1, Value *ptr2,
                   Instruction *loc2, int64_t offset1 = 0,
                   int64_t offset2 = 0);
  bool isSameValue(LoadInst *l1, LoadInst *l2);

  /// Return the path condition under which two pointer values may designate
  /// the same concrete location.  Conditions for every shared guarded
  /// points-to target are combined directly, as in Tuna's aliasCond.
  path_cond_t getAliasCondition(Value *ptr1, Value *ptr2,
                                int64_t offset1 = 0, int64_t offset2 = 0);

  /// Exact satisfiability check for getAliasCondition() with short-circuiting
  /// and revision-aware memoization.
  bool mayAlias(Value *ptr1, Value *ptr2, int64_t offset1 = 0,
                int64_t offset2 = 0);

  /// Conservatively prove that two pointers have syntactically equivalent
  /// guarded points-to sets.  A compact fingerprint rejects the common
  /// non-alias case; exact comparison on a hash match keeps the proof sound
  /// even in the presence of hash collisions.
  bool areMustAliases(Value *ptr1, Value *ptr2, int64_t offset1 = 0,
                      int64_t offset2 = 0);

  const std::set<LoadInst *, llvm_cmp> &
  getAllLoadWithSameValue(LoadInst *load_inst);

  PTResult *findPTResult(Value *ptr, bool is_create = false);

  // Get access-path depth of object to call arguments
  int getObjectToCallApDepth(MemObject *obj, CallInst *call);

  // Utilities
  const DataLayout &getDL();
  Function *getFunc() { return analyzed_func; }
  DominatorTree *getDomTree() { return dom_tree; }
  PTResult *getNullPTS() { return NullPTS; }

  int getObjectID(MemObject *obj) {
    auto it = mem_objs.find(obj);
    assert(it != mem_objs.end() && "Object not in this PTG");
    return it->second;
  }

  void dumpMemObjs();

  path_cond_t getEmptyCond();
  path_cond_t getFalseCond();
  bool isAlwaysSatisfied(path_cond_t cond) const;
  bool isSatisfiable(path_cond_t cond) const;
  bool isNoEffectFunction(Function *F) const;
  path_cond_t getValueCond(Value *value, bool sense = true);
  path_cond_t getBlockCond(BasicBlock *BB);
  path_cond_t getUnitRegion(BasicBlock *BB);
  path_cond_t getCallTargetCond(Value *called_value, Function *callee);
  path_cond_t findOrCreateBBRegion(BasicBlock *src_bb, BasicBlock *target_bb);
  path_cond_t findOrCreateAndRegion(path_cond_t lhs, path_cond_t rhs);
  path_cond_t findOrCreateOrRegion(path_cond_t lhs, path_cond_t rhs);
  path_cond_t findOrCreateNotRegion(path_cond_t cond);
  path_cond_t findOrCreateUnitPhiRegion(BasicBlock *cur_bb,
                                        BasicBlock *incoming_bb);

  friend class MemObject;
  friend class ObjectLocator;
  friend class PTResultIterator;

  static Type *DEFAULT_POINTER_TYPE;
  static Type *DEFAULT_NON_POINTER_TYPE;
  static constexpr int64_t UNKNOWN_OFFSET = std::numeric_limits<int64_t>::max();

  static bool isUnknownOffset(int64_t offset) {
    return offset == UNKNOWN_OFFSET;
  }

  static int64_t composeOffset(int64_t base, int64_t delta) {
    if (isUnknownOffset(base) || isUnknownOffset(delta))
      return UNKNOWN_OFFSET;
    return base + delta;
  }
};

} // namespace llvm
