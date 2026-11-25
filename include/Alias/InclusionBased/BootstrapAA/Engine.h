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

// Chunked sparse bitvector points-to set with small-buffer inline optimization.
// - Empty set is bottom; {UNKNOWN} is top (not an ordinary allocation site).
// - NULL_OBJECT (1) and Top (UNKNOWN == 0) are stored as O(1) header flags, so
//   empty, top, singletons, nullable singletons, and any <= 2-word sparse sets
//   require zero heap allocations (sizeof(PointsToSet) == 32 bytes).
// - Larger sets spill to a sorted contiguous array of 256-bit (4x64-bit) chunks
//   with O(1) locality-cached lookups and in-place zero-allocation fixpoint joins.
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
    static constexpr std::uint32_t POS_TOP = 0xFFFFFFFDu;
    static constexpr std::uint32_t POS_NULL = 0xFFFFFFFEu;
    static constexpr std::uint32_t POS_END = 0xFFFFFFFFu;
    const_iterator(const PointsToSet *set, std::uint32_t pos,
                   std::uint64_t remaining);
    const PointsToSet *m_set = nullptr;
    std::uint32_t m_pos = POS_END;
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

  PointsToSet() noexcept : m_header(0), m_size(0) { m_storage.inl = {}; }
  explicit PointsToSet(Id object) : m_header(0), m_size(0) {
    m_storage.inl = {};
    insert(object);
  }
  PointsToSet(const PointsToSet &other);
  PointsToSet(PointsToSet &&other) noexcept;
  PointsToSet &operator=(const PointsToSet &other);
  PointsToSet &operator=(PointsToSet &&other) noexcept;
  ~PointsToSet();

  static PointsToSet top() noexcept {
    PointsToSet s;
    s.m_header = FLAG_TOP;
    s.m_size = 1;
    return s;
  }
  void clear() noexcept;
  bool isTop() const noexcept { return (m_header & FLAG_TOP) != 0; }
  bool empty() const noexcept { return m_size == 0; }
  std::size_t size() const noexcept { return m_size; }
  bool hasNull() const noexcept { return (m_header & FLAG_NULL) != 0; }
  bool hasNonNull() const noexcept {
    return isTop() || m_size > (hasNull() ? 1u : 0u);
  }
  bool contains(Id object) const;
  bool insert(Id object);
  bool join(const PointsToSet &other);
  bool intersects(const PointsToSet &other) const;
  bool intersectsNonNull(const PointsToSet &other) const;
  ObjectRange objects() const { return ObjectRange(*this); }
  const_iterator begin() const;
  const_iterator end() const;
  bool operator==(const PointsToSet &rhs) const;
  bool operator!=(const PointsToSet &rhs) const { return !(*this == rhs); }
  bool operator<(const PointsToSet &rhs) const;

private:
  static constexpr unsigned WORD_BITS = 64;
  static constexpr unsigned CHUNK_WORDS = 4;
  static constexpr unsigned CHUNK_BITS = WORD_BITS * CHUNK_WORDS;
  static constexpr std::uint32_t FLAG_TOP = 1u << 31;
  static constexpr std::uint32_t FLAG_NULL = 1u << 30;
  static constexpr std::uint32_t FLAG_HEAP = 1u << 29;
  static constexpr std::uint32_t COUNT_MASK = (1u << 29) - 1u;

  struct HeapChunk {
    Id index = 0;
    std::uint32_t pad = 0;
    std::uint64_t words[CHUNK_WORDS] = {0, 0, 0, 0};
  };

  struct InlineStorage {
    Id idx[2] = {0, 0};
    std::uint64_t bits[2] = {0, 0};
  };

  struct HeapStorage {
    HeapChunk *chunks = nullptr;
    std::uint32_t capacity = 0;
    mutable std::uint32_t last_hit = 0;
    std::uint64_t pad = 0;
  };

  union Storage {
    InlineStorage inl;
    HeapStorage heap;
    Storage() noexcept : inl{} {}
  };

  bool isHeap() const noexcept { return (m_header & FLAG_HEAP) != 0; }
  std::uint32_t count() const noexcept { return m_header & COUNT_MASK; }
  void setCount(std::uint32_t c) noexcept {
    m_header = (m_header & ~COUNT_MASK) | (c & COUNT_MASK);
  }
  void releaseHeap() noexcept;
  void setTop() noexcept;
  std::uint32_t heapFindChunk(Id chunk_idx) const noexcept;
  void heapGrow(std::uint32_t min_cap);
  bool heapInsertWord(Id word_idx, std::uint64_t mask, bool update_size = true);
  void promoteInlineToHeap(Id extra_word_idx, std::uint64_t extra_mask);
  bool insertWord(Id word_idx, std::uint64_t mask);

  std::uint32_t m_header = 0;
  std::uint32_t m_size = 0;
  Storage m_storage;
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
  // For Opcode::Havoc: true iff the write fills every covered byte with zero
  // (e.g., memset(dst, 0, len) in address space 0 when null is invalid), so
  // completely-covered aligned pointer fields become NULL_OBJECT instead of Top.
  bool zero_fill = false;
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
  // False only when proven never address-taken (cannot be targeted by an
  // indirect call even if the indirect callee expression widens to Top).
  bool address_taken = true;
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
  // For a zero-fill Havoc of `write_bytes` starting at aligned `od`, returns
  // true iff every overlapping occurrence of `fd` is completely covered (no
  // partial pointer overwrite), so `fd` receives NULL_OBJECT instead of Top.
  bool zeroFillCoversField(Id od, Id fd, std::uint64_t write_bytes) const;
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

enum class HavocPolicy {
  // Default practical mode: treat unmodeled external calls, inline asm, opaque
  // indirect-call fallbacks, and non-pointer / unmodeled memory-havoc writes
  // as no-ops while keeping modeled allocators, memcpy/memmove, zero-fill
  // memset/bzero, pointer stores, and modeled libc return/callback effects.
  Noop,
  // Strictly sound conservative mode: unmodeled external calls, opaque call
  // targets, non-pointer overwrites, and unknown/partial writes havoc
  // potentially affected memory (or all memory when the address is Top) to Top.
  Conservative
};

struct Options {
  SolverMode solver_mode = SolverMode::GuardedSummary;
  HavocPolicy havoc_policy = HavocPolicy::Noop;
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
enum class ContextMatch { Full, Suffix };
enum class AliasKind { NoAlias, MayAlias, PartialAlias, MustAlias };

struct QueryResult {
  PointsToSet points_to;
  QueryStatus status = QueryStatus::Unreachable;
  bool reachable() const { return status != QueryStatus::Unreachable; }
};

struct AliasResult {
  std::vector<Id> aliases;
  // Aliased addressable memory pointer objects (`Mem(obj)`), matching C
  // pointer variables in Kahlon PLDI '08 (e.g., Fig. 5 globals `a, b, c, d`).
  std::vector<Id> object_aliases;
  bool may_alias_all = false;
  QueryStatus status = QueryStatus::Unreachable;
  bool reachable() const { return status != QueryStatus::Unreachable; }
};

struct CallTargets {
  std::vector<Id> functions;
  bool may_call_unknown = false;
  QueryStatus status = QueryStatus::Complete;
  bool reachable() const { return status != QueryStatus::Unreachable; }
};

struct ModRefResult {
  PointsToSet modified;
  PointsToSet referenced;
  bool may_mod_unknown = false;
  bool may_ref_unknown = false;
};

struct ReversePointsToResult {
  std::vector<Id> values;
  std::vector<Id> memory_objects;
  bool may_be_pointed_by_unknown = false;
};

// Context is a sequence of call-site IDs (caller-to-callee).
// With ContextMatch::Full (default), the sequence must start in Program::entry
// (an empty context queries the entry activation).
// With ContextMatch::Suffix, the sequence matches the union of all reachable
// entry-rooted call stacks ending with `context` (empty matches all contexts).
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
                       Point point = Point::Before,
                       ContextMatch match = ContextMatch::Full);
  QueryResult pointsToAllContexts(Id value, Id site,
                                  Point point = Point::Before);
  // Direct may-alias set computation (Algorithm 3 Step 2: backward A + forward
  // Q, unioned over all clusters in the Disjunctive Alias Cover per Theorem 7).
  AliasResult aliases(Id value, Id site, const Context &context = {},
                      Point point = Point::Before,
                      ContextMatch match = ContextMatch::Full);
  AliasResult aliasesAllContexts(Id value, Id site,
                                 Point point = Point::Before);
  // Direct queries over addressable memory pointer objects (`Mem(object)`),
  // matching C pointer variables in Kahlon PLDI '08 (e.g., Fig. 5 `a, b, c, d`).
  QueryResult objectPointsTo(Id object, Id site, const Context &context = {},
                             Point point = Point::Before,
                             ContextMatch match = ContextMatch::Full);
  QueryResult objectPointsToAllContexts(Id object, Id site,
                                        Point point = Point::Before);
  AliasResult objectAliases(Id object, Id site, const Context &context = {},
                            Point point = Point::Before,
                            ContextMatch match = ContextMatch::Full);
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
  bool mayAlias(Id lhs, Id rhs, Id site, const Context &context = {},
                ContextMatch match = ContextMatch::Full);
  // Cross-site may-alias helpers (all-contexts and explicit per-side contexts).
  bool mayAliasAcrossSites(Id lhs, Id lhs_site, Id rhs, Id rhs_site,
                           Point lhs_point = Point::Before,
                           Point rhs_point = Point::Before);
  bool mayAliasAcrossSites(Id lhs, Id lhs_site, const Context &lhs_context,
                           Id rhs, Id rhs_site, const Context &rhs_context,
                           Point lhs_point = Point::Before,
                           Point rhs_point = Point::Before,
                           ContextMatch lhs_match = ContextMatch::Full,
                           ContextMatch rhs_match = ContextMatch::Full);
  // Four-state byte-range-aware alias query (size == 0 means unknown/whole
  // object access).
  AliasKind queryAlias(Id lhs, std::uint64_t lhs_size, Id lhs_site, Id rhs,
                       std::uint64_t rhs_size, Id rhs_site,
                       Point lhs_point = Point::Before,
                       Point rhs_point = Point::Before);
  AliasKind queryAlias(Id lhs, std::uint64_t lhs_size, Id lhs_site,
                       const Context &lhs_context, Id rhs,
                       std::uint64_t rhs_size, Id rhs_site,
                       const Context &rhs_context,
                       Point lhs_point = Point::Before,
                       Point rhs_point = Point::Before,
                       ContextMatch lhs_match = ContextMatch::Full,
                       ContextMatch rhs_match = ContextMatch::Full);
  // Sound must-alias query helper (§1, p. 250): returns true only when lhs and
  // rhs are proven to hold the exact same address on all paths reaching
  // (site, context).
  bool mustAlias(Id lhs, Id rhs, Id site, const Context &context = {},
                 ContextMatch match = ContextMatch::Full);
  // Nullability query helpers (true if reachable and points-to includes
  // NULL_OBJECT or Top).
  bool mayNull(Id value, Id site, Point point = Point::Before);
  bool mayNull(Id value, Id site, const Context &context,
               Point point = Point::Before,
               ContextMatch match = ContextMatch::Full);
  // Derived call-graph, Mod/Ref, and reverse points-to queries.
  CallTargets callTargets(Id call_site);
  CallTargets callTargets(Id call_site, const Context &context,
                          ContextMatch match = ContextMatch::Full);
  std::vector<Id> callersOf(Id function) const;
  bool isRecursiveFunction(Id function) const;
  ModRefResult functionModRef(Id function);
  ModRefResult callModRef(Id call_site);
  ModRefResult callModRef(Id call_site, const Context &context,
                          ContextMatch match = ContextMatch::Full);
  ReversePointsToResult reversePointsTo(Id object);
  const Object &objectInfo(Id object) const;
  const Statistics &statistics() const;
  const std::vector<std::vector<Id>> &clusters() const;
  const SteensgaardHierarchy &hierarchy() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace bootstrap
} // namespace lotus
