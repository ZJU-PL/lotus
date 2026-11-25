#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lotus {
namespace bootstrap {

using Id = std::uint32_t;
constexpr Id INVALID = std::numeric_limits<Id>::max();
constexpr Id UNKNOWN = 0;
constexpr Id NULL_OBJECT = 1;

// The empty set is bottom; {UNKNOWN} is top, not an ordinary allocation site.
class PointsToSet {
public:
  class const_iterator {
  public:
    using value_type = Id;
    using difference_type = std::ptrdiff_t;
    using pointer = const Id *;
    using reference = Id;
    using iterator_category = std::forward_iterator_tag;

    const_iterator() = default;
    Id operator*() const;
    const_iterator &operator++();
    bool operator==(const const_iterator &other) const;
    bool operator!=(const const_iterator &other) const {
      return !(*this == other);
    }

  private:
    friend class PointsToSet;
    const_iterator(const PointsToSet *set, std::size_t word,
                   std::uint64_t remaining);
    const PointsToSet *m_set = nullptr;
    std::size_t m_word = 0;
    std::uint64_t m_remaining = 0;
  };

  class ObjectRange {
  public:
    explicit ObjectRange(const PointsToSet &set) : m_set(&set) {}
    const_iterator begin() const;
    const_iterator end() const;

  private:
    const PointsToSet *m_set;
  };

  PointsToSet() = default;
  explicit PointsToSet(Id object) { insert(object); }
  static PointsToSet top() { return PointsToSet(UNKNOWN); }
  bool isTop() const;
  bool empty() const { return m_words.empty(); }
  std::size_t size() const;
  bool contains(Id object) const;
  bool insert(Id object);
  bool join(const PointsToSet &other);
  bool intersects(const PointsToSet &other) const;
  ObjectRange objects() const { return ObjectRange(*this); }
  const_iterator begin() const;
  const_iterator end() const;
  bool operator==(const PointsToSet &rhs) const;
  bool operator!=(const PointsToSet &rhs) const { return !(*this == rhs); }
  bool operator<(const PointsToSet &rhs) const;

private:
  struct Word {
    Id index = 0;
    std::uint64_t bits = 0;
    bool operator==(const Word &other) const {
      return index == other.index && bits == other.bits;
    }
  };
  static constexpr unsigned WORD_BITS = 64;
  std::vector<Word> m_words;
};

enum class ObjectKind { Unknown, Null, Global, Stack, Heap, Function };

// Collapsed array region inside a flattened aggregate object: accesses to any
// byte offset in [begin, end) fold modulo stride to begin + ((off-begin)%stride).
struct ArrayRegion {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;
  std::uint64_t stride = 0;
};

struct Object {
  std::string name;
  ObjectKind kind = ObjectKind::Heap;
  // True only for one concrete, whole pointer cell. Never true for summary
  // heaps, aggregate objects, array elements, loop allocations, or
  // recursive-frame allocations.
  bool singleton = false;
  std::uint64_t bytes = 0; // 0 means an indivisible cell or non-pointer field.
  PointsToSet initial = PointsToSet::top();
  Id function = INVALID;

  // Field-sensitivity metadata (Remark 1 struct flattening).
  // For root objects, base_object == INVALID (meaning self).
  // For flattened field objects, base_object is the root allocation object ID
  // and offset is the byte offset within base_object.
  Id base_object = INVALID;
  std::uint64_t offset = 0;
  // Total byte span of the allocation (on root objects) or field span (on
  // flattened fields). 0 means unspecified / indivisible.
  std::uint64_t size_bytes = 0;
  bool field_sensitive = false;
  bool has_interior_pointer = false;
  std::vector<ArrayRegion> array_regions;
};

struct Value {
  std::string name;
  Id function = INVALID;
  bool constant = false;
  PointsToSet initial;
};

enum class Opcode {
  Nop,
  Address,
  Allocate,
  Copy,
  Join,
  Field,
  Load,
  Store,
  MemCopy,
  Havoc,
  Unknown,
  Call,
  Return
};

struct Instruction {
  Id id = INVALID;
  Opcode opcode = Opcode::Nop;
  Id result = INVALID;
  // Copy/Join/Load/Field: sources. Store: {address, value}.
  // MemCopy: {dest_address, src_address}. Havoc: optional address.
  // Call: positional actual arguments, including INVALID for non-pointer args.
  std::vector<Id> operands;
  Id object = INVALID;
  bool may_be_null = false; // Allocation failure is not a fresh object.
  Id callee = INVALID;      // Direct callee, otherwise indirect_target.
  Id indirect_target = INVALID;
  bool opaque_alternative = false; // EH/callbr: conservative additional path.
  bool writes_memory = true;       // Honored for opaque/external calls.
  std::uint64_t write_bytes = 0;
  std::uint64_t read_bytes = 0;
  // For Opcode::Field: byte offset relative to operands[0]. When field_known is
  // false, represents an inbounds variable-offset access within the same base
  // allocation (or collapsed array stride when array_stride > 0).
  std::int64_t field_offset = 0;
  bool field_known = true;
  std::uint64_t field_array_stride = 0;
  // Optional condition variable(s) defined/killed by this instruction (for
  // path sensitivity: existentially quantifies these boolean predicates when
  // stepping backwards across loop backedges or global flag writes).
  Id defines_condition = INVALID;
  std::vector<Id> kills_conditions;
  // Optional path-sensitivity condition for 2-operand Opcode::Join (SelectInst):
  // operands[0] is taken when cond == polarity; operands[1] when negated.
  Id select_cond_var = INVALID;
  bool select_cond_polarity = true;
  // Optional interprocedural formal-to-actual condition substitution for
  // Opcode::Call: maps a callee parameter-scoped condition atom ID to either a
  // constant boolean (caller_cond == INVALID) or a caller CondNode expression.
  struct CondSubst {
    Id callee_cond = INVALID;
    Id caller_cond = INVALID;
    bool polarity = true;
  };
  std::vector<CondSubst> cond_subst = {};
};

enum class EdgeGuardKind : std::uint8_t {
  PointsTo,
  NotPointsTo,
  Alias,
  NotAlias
};

// Pointer-comparison branch guard on a CFG edge (e.g., `if (p == &a)`,
// `if (p != NULL)`, `if (p == q)`), bridging path sensitivity with Definition 8
// pointer constraints.
struct EdgePtrGuard {
  EdgeGuardKind kind = EdgeGuardKind::PointsTo;
  Id lhs = INVALID;
  Id rhs = INVALID; // Object ID for PointsTo/NotPointsTo; Value ID for Alias/NotAlias
  Id site = INVALID;
};

struct Edge {
  Id target = INVALID;
  // All assignments on an edge are simultaneous (LLVM PHI semantics).
  std::vector<std::pair<Id, Id>> phi = {};
  // Optional path-sensitivity branch condition (Section 3, p. 258):
  // cond_var == INVALID means unconditional edge. References Program::cond_nodes.
  Id cond_var = INVALID;
  bool cond_polarity = true;
  // Optional pointer-level branch guards on this edge.
  std::vector<EdgePtrGuard> ptr_guards = {};
};

enum class CondOp : std::uint8_t { Atom, Not, And, Or };
struct CondNode {
  CondOp op = CondOp::Atom;
  Id lhs = INVALID;
  Id rhs = INVALID;
};

struct Block {
  std::vector<Instruction> instructions;
  std::vector<Edge> successors;
};

struct Function {
  std::string name;
  std::vector<Id> parameters;
  std::vector<Block> blocks;
  bool external = false;
  bool writes_memory = true;
  // Condition variables scoped to a single activation of this function (e.g.,
  // predicates over its own formal arguments) that must be existentially
  // quantified when crossing this function's entry boundary.
  std::vector<Id> entry_kills_conditions;
};

// Small LLVM-independent IR. All IDs are stable vector indices except site IDs,
// which are assigned by append(). Finish construction before creating Analysis.
struct Program {
  Program();
  std::vector<Object> objects;
  std::vector<Value> values;
  std::vector<Function> functions;
  Id entry = INVALID;

  // Map from base object ID to sorted (byte_offset, field_object_id) entries
  // for flattened field-sensitive aggregates (including base itself at offset
  // 0).
  std::vector<std::vector<std::pair<std::uint64_t, Id>>> fields_of_base;
  // Boolean condition expression table (atoms + compound Not/And/Or formulas)
  // used for ROBDD path sensitivity (Section 3, p. 258).
  std::vector<CondNode> cond_nodes;
  // Number of condition nodes (equal to cond_nodes.size()).
  Id condition_count = 0;
  // Condition variables backed by mutable global storage (quantified across
  // opaque or memory-writing external calls).
  std::vector<Id> global_conditions;

  Id addObject(Object object);
  Id addFieldObject(Id base, std::uint64_t offset, Object object);
  Id addValue(Value value);
  Id addConstant(PointsToSet points_to, std::string name = {});
  Id addFunction(std::string name, bool external = false);
  Id addBlock(Id function);
  Id append(Id function, Id block, Instruction instruction);
  Id addCondition();
  Id addCondNot(Id operand);
  Id addCondAnd(Id lhs, Id rhs);
  Id addCondOr(Id lhs, Id rhs);

  // Field-resolution helpers.
  Id baseOf(Id object) const;
  std::uint64_t foldOffset(Id base, std::uint64_t offset) const;
  // Resolve applying a byte offset delta (or unknown offset) to an object.
  PointsToSet resolveField(Id object, std::int64_t delta, bool known,
                           std::uint64_t array_stride = 0) const;
  // Collect all flattened field objects of baseOf(object) whose byte ranges
  // overlap [offset(object), offset(object) + bytes). bytes == 0 means entire
  // object / all fields of base.
  std::vector<Id> overlappingFields(Id object, std::uint64_t bytes) const;
  // For a MemCopy of `write_bytes` into `od`, compute every exact relative
  // source byte offset that writes into overlapping field `fd`. Returns false
  // if any overlapping write is partial or ambiguous (caller must emit Top).
  bool memCopyRelativeOffsets(Id od, Id fd, std::uint64_t write_bytes,
                              std::vector<std::int64_t> &rels) const;
  void validate() const;

private:
  Id m_next_site = 0;
};

enum class SolverMode {
  // Faithful Kahlon (PLDI '08) Section 3 solver: top-down Steensgaard-depth
  // dovetailing (Alg. 2), guarded backward update-sequence summaries (Def. 8,
  // Algs. 4-5), and FSCI/FSCS summary splicing (Alg. 3) with optional ROBDD
  // path sensitivity.
  GuardedSummary,
  // Reference forward input-state tabulation solver (useful as a differential
  // oracle and fallback verification backend).
  InputStateTabulation
};

struct Options {
  SolverMode solver_mode = SolverMode::GuardedSummary;
  std::size_t andersen_threshold = 60;
  bool adaptive_andersen_threshold = true;
  // When true (default), rejecting a large high-overlap partition escalates the
  // threshold globally; when false, rejection is strictly per-partition so
  // smaller partitions with low overlap still undergo Andersen refinement.
  bool adaptive_global_threshold_escalation = true;
  // Reject Andersen split on a partition if largest_cluster / partition_size
  // meets or exceeds this retention ratio (paper §4 p. 259 overlap criterion).
  double andersen_rejection_retention_ratio = 0.75;
  // Zero disables this adaptive preprocessing cost guard.
  std::size_t max_andersen_partition_size = 4096;
  // Estimated partition-size x hierarchy-node budget; zero is unlimited.
  std::size_t max_andersen_work = 4 * 1024 * 1024;
  // Optional Das (2000) one-level-flow intermediate cascade stage between
  // Steensgaard partitioning and Andersen clustering (paper §4, p. 258).
  bool enable_one_level_flow = false;
  // Optional hybrid per-cluster solver selection (paper §1, p. 250):
  // when > 0, clusters of size <= hybrid_tabulation_max_cluster_size (and, if
  // hybrid_tabulation_max_slice_statements > 0, slice statements <= that bound)
  // use InputStateTabulation while remaining clusters use solver_mode.
  std::size_t hybrid_tabulation_max_cluster_size = 0;
  std::size_t hybrid_tabulation_max_slice_statements = 0;
  // Optional custom per-cluster solver selector callback:
  // (cluster_index, cluster_size, slice_statements) -> SolverMode.
  std::function<SolverMode(std::size_t, std::size_t, std::size_t)>
      cluster_solver_selector;
  // Optional ROBDD-based path sensitivity for summary tuples (paper §3, p. 258).
  bool path_sensitive = false;
  bool path_sensitive_interprocedural = false;
  // Literal Algorithm 2 eager summary precomputation across Steensgaard depths
  // (in reverse topological SCC order); false keeps summaries demand-driven.
  bool eager_dovetail_summaries = false;
  // Sound guard widening bounds for GuardedSummary mode (dropping guard atoms
  // weakens constraints and preserves may-alias soundness).
  std::size_t max_guard_atoms = 8;
  std::size_t max_guards_per_state = 16;
  std::size_t max_bdd_nodes = 65536;
  bool parallel_clusters = true;
  // Zero selects hardware_concurrency. Values above one enable parallel work.
  std::size_t parallelism = 0;
  // Resource exhaustion yields top plus an explicit status, never partial sets.
  std::size_t max_contexts = 4096;
  std::size_t max_steps = 1000000;
  bool enable_slicing = true; // Useful for differential testing.
  bool enable_clustering = true;
};

struct SteensgaardHierarchy {
  // Dense component IDs for normalized values and allocation objects.
  std::vector<Id> value_components;
  std::vector<Id> object_components;
  // Cycle-collapsed points-to hierarchy. Edges run from a pointer partition
  // to the partition reached by one dereference.
  std::vector<std::vector<Id>> successors;
  std::vector<std::vector<Id>> predecessors;
  std::vector<std::vector<Id>> values;
  std::vector<std::size_t> depth;
  std::vector<bool> cyclic;
  Id top_component = INVALID;

  bool isHigher(Id higher, Id lower) const;
};

struct Statistics {
  std::size_t steensgaard_partitions = 0;
  std::size_t steensgaard_largest_partition = 0;
  std::vector<std::size_t> steensgaard_partition_sizes;
  std::size_t hierarchy_nodes = 0;
  std::size_t hierarchy_edges = 0;
  std::size_t hierarchy_max_depth = 0;
  std::size_t hierarchy_cyclic_components = 0;
  std::size_t one_level_flow_runs = 0;
  std::size_t andersen_runs = 0;
  std::size_t adaptive_refinement_skips = 0;
  std::size_t adaptive_refinement_rejections = 0;
  std::size_t adaptive_cost_skips = 0;
  std::size_t effective_andersen_threshold = 0;
  std::size_t clusters = 0;
  std::size_t largest_cluster = 0;
  std::vector<std::size_t> cluster_sizes;
  std::size_t cover_memberships = 0;
  std::size_t overlapping_values = 0;
  std::size_t maximum_cover_memberships = 0;
  std::size_t slice_values = 0;
  std::size_t slice_objects = 0;
  std::size_t slice_statements = 0;
  std::size_t evaluated_clusters = 0;
  std::size_t parallel_cluster_tasks = 0;
  std::size_t call_graph_sccs = 0;
  std::size_t recursive_call_graph_sccs = 0;
  std::size_t scc_reschedules = 0;
  std::size_t contexts = 0;
  std::size_t context_cache_hits = 0;
  std::size_t context_cache_misses = 0;
  std::size_t maximum_contexts_per_cluster = 0;
  std::size_t summary_updates = 0;
  // Paper Section 3 (Algorithms 2-5 & Path Sensitivity) counters.
  std::size_t dovetail_levels = 0;
  std::size_t fsci_queries = 0;
  std::size_t fsci_cache_hits = 0;
  std::size_t summary_tuples = 0;
  std::size_t guard_atoms_created = 0;
  std::size_t guards_pruned = 0;
  std::size_t guards_widened = 0;
  std::size_t skipped_functions = 0;
  std::size_t path_infeasible_prunes = 0;
  std::size_t bdd_nodes = 0;
  double preprocessing_milliseconds = 0.0;
  std::vector<double> cluster_solve_milliseconds;
  std::size_t steps = 0;
  std::size_t fallback_queries = 0;
};

enum class QueryStatus { Complete, Unreachable, ResourceLimit };
enum class Point { Before, After };
struct QueryResult {
  PointsToSet points_to;
  QueryStatus status = QueryStatus::Unreachable;
  bool reachable() const { return status != QueryStatus::Unreachable; }
};

struct AliasResult {
  std::vector<Id> aliases;
  bool may_alias_all = false;
  QueryStatus status = QueryStatus::Unreachable;
  bool reachable() const { return status != QueryStatus::Unreachable; }
};

// Context is a complete sequence of call-site IDs starting in Program::entry.
// An empty context queries the entry activation, not a union over all callers.
using Context = std::vector<Id>;
class Analysis {
public:
  explicit Analysis(const Program &program, Options options = {});
  ~Analysis();
  Analysis(Analysis &&) noexcept;
  Analysis &operator=(Analysis &&) noexcept;
  Analysis(const Analysis &) = delete;
  Analysis &operator=(const Analysis &) = delete;

  QueryResult pointsTo(Id value, Id site, const Context &context = {},
                       Point point = Point::Before);
  QueryResult pointsToAllContexts(Id value, Id site,
                                  Point point = Point::Before);
  // Direct may-alias set computation (Algorithm 3 Step 2: backward A + forward
  // Q, unioned over all clusters in the Disjunctive Alias Cover per Theorem 7).
  AliasResult aliases(Id value, Id site, const Context &context = {},
                      Point point = Point::Before);
  AliasResult aliasesAllContexts(Id value, Id site,
                                 Point point = Point::Before);
  // Direct queries over addressable memory pointer objects (`Mem(object)`),
  // matching C pointer variables in Kahlon PLDI '08 (e.g., Fig. 5 `a, b, c, d`).
  QueryResult objectPointsTo(Id object, Id site, const Context &context = {},
                             Point point = Point::Before);
  QueryResult objectPointsToAllContexts(Id object, Id site,
                                        Point point = Point::Before);
  AliasResult objectAliases(Id object, Id site, const Context &context = {},
                            Point point = Point::Before);
  AliasResult objectAliasesAllContexts(Id object, Id site,
                                       Point point = Point::Before);
  // Eagerly construct every cluster solver, using configured parallelism.
  void precomputeAll();
  // Selective application-driven cluster exploration (§1, p. 250): eagerly
  // build only the cluster solvers covering `values` or `cluster_indices`.
  void precomputeForValues(const std::vector<Id> &values);
  void precomputeClusters(const std::vector<std::size_t> &cluster_indices);
  std::vector<std::size_t> clustersFor(Id value) const;
  // Conservative may-alias query helper (consistent with non-null object
  // aliasing; unreachable is not used to prove disjointness).
  bool mayAlias(Id lhs, Id rhs, Id site, const Context &context = {});
  // Sound must-alias query helper (§1, p. 250): returns true only when lhs and
  // rhs are proven to hold the exact same address on all paths reaching
  // (site, context).
  bool mustAlias(Id lhs, Id rhs, Id site, const Context &context = {});
  const Statistics &statistics() const;
  const std::vector<std::vector<Id>> &clusters() const;
  const SteensgaardHierarchy &hierarchy() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace bootstrap
} // namespace lotus
