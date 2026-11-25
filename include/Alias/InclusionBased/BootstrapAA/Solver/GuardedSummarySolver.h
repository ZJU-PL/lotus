#pragma once

#include "Alias/InclusionBased/BootstrapAA/Solver/ClusterSolver.h"
#include "Alias/InclusionBased/BootstrapAA/Support/Bdd.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace lotus {
namespace bootstrap {
namespace detail {

// Tracked pointer expression during backward maximally-complete update-sequence
// traversal (Kahlon PLDI '08, Definitions 3-4, 8 & Algorithms 4-5).
enum class ExprKind : std::uint8_t {
  Val,   // SSA value / formal / constant v (+ val_offset)
  Mem,   // Contents of memory object o (+ val_offset)
  Deref, // *s: contents of memory pointed to by s (+ deref_offset), shifted by
         //     val_offset, with read_bytes
  Addr,  // &o: constant address of object o (terminal source)
  Null,  // NULL_OBJECT (terminal source)
  Top    // UNKNOWN (terminal source)
};

struct Expr {
  ExprKind kind = ExprKind::Top;
  Id id = INVALID; // Value ID for Val/Deref, Object ID for Mem/Addr
  std::int64_t val_offset = 0;
  bool val_offset_known = true;
  std::uint64_t val_stride = 0;
  std::int64_t deref_offset = 0;
  bool deref_offset_known = true;
  std::uint64_t deref_stride = 0;
  std::uint64_t read_bytes = 0;

  static Expr makeVal(Id v, std::int64_t off = 0, bool known = true,
                      std::uint64_t stride = 0) {
    Expr e;
    e.kind = ExprKind::Val;
    e.id = v;
    e.val_offset = (known || stride != 0) ? off : 0;
    e.val_offset_known = known;
    e.val_stride = stride;
    return e;
  }
  static Expr makeMem(Id o, std::int64_t off = 0, bool known = true,
                      std::uint64_t stride = 0) {
    Expr e;
    e.kind = ExprKind::Mem;
    e.id = o;
    e.val_offset = (known || stride != 0) ? off : 0;
    e.val_offset_known = known;
    e.val_stride = stride;
    return e;
  }
  static Expr makeDeref(Id s, std::int64_t doff = 0, bool dknown = true,
                        std::uint64_t dstride = 0, std::int64_t voff = 0,
                        bool vknown = true, std::uint64_t vstride = 0,
                        std::uint64_t rbytes = 0) {
    Expr e;
    e.kind = ExprKind::Deref;
    e.id = s;
    e.val_offset = (vknown || vstride != 0) ? voff : 0;
    e.val_offset_known = vknown;
    e.val_stride = vstride;
    e.deref_offset = (dknown || dstride != 0) ? doff : 0;
    e.deref_offset_known = dknown;
    e.deref_stride = dstride;
    e.read_bytes = rbytes;
    return e;
  }
  static Expr makeAddr(Id o) {
    Expr e;
    e.kind = ExprKind::Addr;
    e.id = o;
    return e;
  }
  static Expr makeNull() {
    Expr e;
    e.kind = ExprKind::Null;
    return e;
  }
  static Expr makeTop() {
    Expr e;
    e.kind = ExprKind::Top;
    return e;
  }
  bool isTerminal() const {
    return kind == ExprKind::Addr || kind == ExprKind::Null ||
           kind == ExprKind::Top;
  }
  bool operator==(const Expr &o) const {
    return std::tie(kind, id, val_offset, val_offset_known, val_stride,
                    deref_offset, deref_offset_known, deref_stride,
                    read_bytes) ==
           std::tie(o.kind, o.id, o.val_offset, o.val_offset_known,
                    o.val_stride, o.deref_offset, o.deref_offset_known,
                    o.deref_stride, o.read_bytes);
  }
  bool operator<(const Expr &o) const {
    return std::tie(kind, id, val_offset, val_offset_known, val_stride,
                    deref_offset, deref_offset_known, deref_stride,
                    read_bytes) <
           std::tie(o.kind, o.id, o.val_offset, o.val_offset_known,
                    o.val_stride, o.deref_offset, o.deref_offset_known,
                    o.deref_stride, o.read_bytes);
  }
};

// Definition 8 (p. 255): four forms of points-to/alias constraints, qualified
// by call-site suffix when spliced across callers so FSCS evaluation resolves
// guards in the exact calling context (ctx ++ call_suffix).
enum class GuardKind : std::uint8_t {
  PointsTo,    // site : lhs -> rhs_obj
  NotPointsTo, // site : lhs -/-> rhs_obj
  Alias,       // site : lhs ~ rhs_val
  NotAlias     // site : lhs !~ rhs_val
};

struct GuardAtom {
  std::vector<Id> call_suffix;
  bool context_insensitive = false;
  Id site = INVALID;
  GuardKind kind = GuardKind::PointsTo;
  Id lhs = INVALID;
  Id rhs = INVALID; // Object ID for PointsTo/NotPointsTo; Value ID for Alias/NotAlias
  std::int64_t lhs_offset = 0;
  bool lhs_offset_known = true;
  std::uint64_t lhs_stride = 0;

  bool operator==(const GuardAtom &o) const {
    return std::tie(call_suffix, context_insensitive, site, kind, lhs, rhs,
                    lhs_offset, lhs_offset_known, lhs_stride) ==
           std::tie(o.call_suffix, o.context_insensitive, o.site, o.kind, o.lhs,
                    o.rhs, o.lhs_offset, o.lhs_offset_known, o.lhs_stride);
  }
  bool operator<(const GuardAtom &o) const {
    return std::tie(call_suffix, context_insensitive, site, kind, lhs, rhs,
                    lhs_offset, lhs_offset_known, lhs_stride) <
           std::tie(o.call_suffix, o.context_insensitive, o.site, o.kind, o.lhs,
                    o.rhs, o.lhs_offset, o.lhs_offset_known, o.lhs_stride);
  }
};

using AtomId = std::uint32_t;
using GuardId = std::uint32_t;
constexpr GuardId GUARD_TRUE = 0;
constexpr GuardId GUARD_FALSE = std::numeric_limits<GuardId>::max();

struct SummaryEntry {
  Expr source;
  GuardId guard = GUARD_TRUE;
  BddManager::NodeId path = BddManager::TRUE_NODE;
  bool operator==(const SummaryEntry &o) const {
    return source == o.source && guard == o.guard && path == o.path;
  }
  bool operator<(const SummaryEntry &o) const {
    return std::tie(source, guard, path) < std::tie(o.source, o.guard, o.path);
  }
};

enum class SummaryKind : std::uint8_t {
  ExitReturn, // Return value summary of function f
  ExitMem,    // Memory object exit summary of function f
  LocalPoint  // Local summary of Expr at (site, point) inside function f
};

struct SummaryKey {
  Id function = INVALID;
  SummaryKind kind = SummaryKind::LocalPoint;
  Expr target;
  Id site = INVALID;
  Point point = Point::Before;

  bool operator==(const SummaryKey &o) const {
    return std::tie(function, kind, target, site, point) ==
           std::tie(o.function, o.kind, o.target, o.site, o.point);
  }
  bool operator<(const SummaryKey &o) const {
    return std::tie(function, kind, target, site, point) <
           std::tie(o.function, o.kind, o.target, o.site, o.point);
  }
};

struct CfgPos {
  Id site = INVALID;
  Point point = Point::Before;
  bool operator==(const CfgPos &o) const {
    return site == o.site && point == o.point;
  }
  bool operator<(const CfgPos &o) const {
    return std::tie(site, point) < std::tie(o.site, o.point);
  }
};

// Guarded backward update-sequence solver faithfully implementing Kahlon
// (PLDI '08) Section 3:
//  - Definition 8: guarded summary tuples (p, loc, q, c_1 ∧ ... ∧ c_k[, con_b])
//  - Algorithm 2: top-down Steensgaard-depth dovetailing with FSCI points-to
//  - Algorithm 3: FSCI/FSCS backward (A) + forward (Q) summary splicing
//  - Algorithm 4: backward tuple transfer over St_P across all IR statements
//  - Algorithm 5: reverse-topological SCC summary computation skipping
//                 untouched functions
//  - Section 3 (p. 258): ROBDD path sensitivity
class GuardedSummarySolver final : public ClusterSolver {
public:
  GuardedSummarySolver(const Program &program, const ProgramIndex &index,
                       const Sets &upper, const SteensgaardHierarchy &hierarchy,
                       const CallGraphInfo &call_graph,
                       const ReachabilityInfo &reachability,
                       std::vector<Id> cluster_values, Slice slice,
                       const Options &options);

  QueryResult query(Id value, Id site, const Context *context_path,
                    Point point) override;
  AliasResult queryAliases(Id value, Id site, const Context *context_path,
                           Point point) override;
  const Statistics &statistics() const override { return m_stats; }

private:
  using FsciKey = std::tuple<Id, Id, Point>;
  using FscsKey = std::tuple<Id, Id, Point, Context>;
  static constexpr std::size_t MAX_PASS1_EXTRA_KEYS = 256;

  struct WorkItem {
    SummaryKey owner;
    CfgPos pos;
    Expr q;
    GuardId guard = GUARD_TRUE;
    BddManager::NodeId path = BddManager::TRUE_NODE;
  };
  struct Waiter {
    SummaryKey owner;
    Id call_site = INVALID;
    Id callee = INVALID;
    std::int64_t caller_off = 0;
    bool caller_known = true;
    std::uint64_t caller_stride = 0;
    GuardId call_guard = GUARD_TRUE;
    BddManager::NodeId call_path = BddManager::TRUE_NODE;
    bool operator<(const Waiter &o) const {
      return std::tie(owner, call_site, callee, caller_off, caller_known,
                      caller_stride, call_guard, call_path) <
             std::tie(o.owner, o.call_site, o.callee, o.caller_off,
                      o.caller_known, o.caller_stride, o.call_guard,
                      o.call_path);
    }
  };

  // --- GuardedSummaryState.cpp: guards, offsets, mod-ref, FSCI/guard eval ---
  void tick();
  void countContext(const SummaryKey &key);
  std::tuple<std::int64_t, bool, std::uint64_t>
  combineOffset(std::int64_t a, bool a_known, std::uint64_t a_stride,
                std::int64_t b, bool b_known, std::uint64_t b_stride) const;
  PointsToSet applyOffset(const PointsToSet &base, std::int64_t off, bool known,
                          std::uint64_t stride) const;
  AtomId internAtom(GuardAtom atom);
  GuardId internGuard(std::vector<AtomId> atoms);
  bool atomsContradict(const GuardAtom &a, const GuardAtom &b) const;
  GuardId conjoinAtom(GuardId base, const GuardAtom &atom);
  GuardId conjoinGuards(GuardId a, GuardId b);
  GuardId prependCallSite(GuardId g, Id call_site);
  bool guardSubsumes(GuardId weaker, GuardId stronger) const;
  void computeFunctionModifications();
  void computeFunctionConditionKills();
  BddManager::NodeId quantifyInstConditions(const Instruction &inst,
                                            BddManager::NodeId path);
  BddManager::NodeId quantifyFuncConditions(Id func, BddManager::NodeId path);
  BddManager::NodeId quantifyFuncEntryConditions(Id func,
                                                 BddManager::NodeId path);
  BddManager::NodeId quantifyGlobalConditions(BddManager::NodeId path);
  bool canFunctionModifyObj(Id f, Id obj) const;
  bool canFunctionModifyAny(Id f, const PointsToSet &objs) const;
  PointsToSet lookupPass1Fsci(Id val, Id site, Point pt);
  PointsToSet fsciForSummary(Id val, Id site, std::size_t target_depth);
  bool isSameOrHigherDepth(Id val, std::size_t target_depth) const;
  PointsToSet evalGuardPointer(Id val, Id site, const Context *ctx,
                               bool for_negative_guard, bool &is_exact_upper);
  bool memoryIntersects(const PointsToSet &a, const PointsToSet &b) const;
  bool isAtomSatisfiable(const GuardAtom &atom, const Context *ctx);
  bool isGuardSatisfiable(GuardId g, const Context *ctx);
  void emitPointsToSources(const PointsToSet &pts, std::int64_t val_off,
                           bool val_known, std::uint64_t val_stride,
                           GuardId guard, BddManager::NodeId path,
                           std::vector<SummaryEntry> &out) const;
  void substituteCalleeExpr(Id callee, const Instruction &call_inst,
                            const Expr &callee_expr, std::int64_t caller_off,
                            bool caller_known, std::uint64_t caller_stride,
                            GuardId guard, BddManager::NodeId path,
                            std::vector<SummaryEntry> &out) const;
  void evaluateAtEntry(const Expr &q, PointsToSet &out) const;

  // --- GuardedSummaryTransfer.cpp: Algorithm 4 transfer & PHI substitution --
  void materializeDeref(const Expr &deref, Id site, std::size_t target_depth,
                        GuardId guard, BddManager::NodeId path,
                        std::vector<SummaryEntry> &out,
                        Point site_point = Point::Before);
  void transferBackward(const Instruction &inst, const Expr &q, GuardId guard,
                        BddManager::NodeId path, std::size_t target_depth,
                        std::vector<SummaryEntry> &out);
  void applyPhiBackward(const Edge &edge, const Expr &q, GuardId guard,
                        BddManager::NodeId path, std::size_t target_depth,
                        Id pred_site, std::vector<SummaryEntry> &out);

  // --- GuardedSummaryEngine.cpp: Alg. 5 SCCs, Alg. 2 dovetailing, Alg. 3 ---
  std::size_t depthOfSummaryKey(const SummaryKey &key) const;
  const std::vector<SummaryEntry> &computeSummary(const SummaryKey &root_key);
  // Algorithm 3 Step 1: backward summary splicing returning both concrete
  // points-to set and entry-level anchors A.
  PointsToSet computeFsci(Id val, Id site, Point point,
                          std::set<Expr> *entry_anchors = nullptr,
                          bool *path_pruned_out = nullptr);
  bool validateContext(Id site, const Context &ctx);
  QueryResult computeFscs(Id val, Id site, const Context &ctx, Point point,
                          std::set<Expr> *entry_anchors = nullptr);
  // Algorithm 3 Step 2: forward summary propagation from entry anchors A to Q.
  void propagateForwardAnchorsFsci(const std::set<Expr> &anchors, Id target_fn,
                                   Id site, Point point,
                                   const std::vector<Id> &candidates,
                                   std::set<Id> &out_aliases);
  void propagateForwardAnchorsFscs(const std::set<Expr> &anchors,
                                   const Context &ctx, Id site, Point point,
                                   const std::vector<Id> &candidates,
                                   std::set<Id> &out_aliases);
  void precomputeDepthSummaries(std::size_t depth);
  void computeFlowSensitiveReachability();
  void runDovetailing();

  const Program &m_program;
  const ProgramIndex &m_index;
  const Sets &m_upper;
  const SteensgaardHierarchy &m_hierarchy;
  const CallGraphInfo &m_call_graph;
  const ReachabilityInfo &m_reachability;
  std::vector<Id> m_cluster_values;
  Slice m_slice;
  const Options &m_options;
  Statistics m_stats;
  BddManager m_bdd;

  std::vector<GuardAtom> m_atoms;
  std::map<GuardAtom, AtomId> m_atom_ids;
  std::vector<std::vector<AtomId>> m_guards;
  std::map<std::vector<AtomId>, GuardId> m_guard_ids;

  Sets m_func_modifies;
  std::vector<bool> m_func_modifies_any;
  std::vector<std::vector<Id>> m_func_kills_conditions;
  std::vector<bool> m_fs_reachable_function;
  std::int64_t m_max_offset_span = 0;

  std::map<SummaryKey, std::vector<SummaryEntry>> m_summaries;
  std::set<SummaryKey> m_finalized;
  std::set<SummaryKey> m_path_pruned_summaries;

  std::map<FsciKey, PointsToSet> m_fsci_cache;
  std::set<FsciKey> m_path_pruned_fsci;
  std::map<FsciKey, PointsToSet> m_cyclic_pass1_fsci;
  std::map<FscsKey, QueryResult> m_fscs_cache;
  std::set<FscsKey> m_active_fscs;
  std::set<FsciKey> m_active_fsci;

  std::size_t m_completed_depth = 0;
  bool m_has_completed_depth = false;
  std::size_t m_cyclic_pass1_depth = std::numeric_limits<std::size_t>::max();
  std::set<FsciKey> m_pass1_keys;
  std::vector<FsciKey> m_pass1_pending_keys;
  std::size_t m_pass1_extra_added = 0;
  std::set<SummaryKey> m_counted_keys;
  std::size_t m_steps = 0;
  std::size_t m_context_budget_used = 0;
  bool m_failed = false;
};

} // namespace detail
} // namespace bootstrap
} // namespace lotus
