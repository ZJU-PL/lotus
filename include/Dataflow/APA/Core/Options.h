#pragma once

#include "Dataflow/APA/EAN/Budget.h"
#include "Dataflow/APA/EAN/CostModel.h"
#include "Dataflow/APA/EAN/ExtractOptions.h"
#include "Dataflow/APA/EAN/LawProfile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace elimination {

enum class EliminationMethod {
  // Generic O(n^3) state-elimination (Floyd–Warshall-style) over all nodes.
  StateElimination,
  // Paper-style ADT "simple" algorithm path-expression updates.
  ADTSimple,
  // Paper-style ADT + path-expression construction (requires reducible info).
  ADTDelayed,
};

// Ordering preserves path languages. Equal client facts additionally require
// a language-invariant interpretation (e.g. a quantale), not merely a lattice.
enum class OrderingPolicy {
  // Baseline: reverse-topological order for reducible intra problems, identity
  // otherwise (unchanged historical behavior). Sparse equation baseline: index.
  Default,
  // Cost-aware greedy minimum-product ordering that minimizes the
  // predecessor–successor product driving Eq. 1's intermediate growth.
  CostAware,
  Structural,
  ExpressionAware,
  StarRisk,
  Hybrid,
  ReversePostOrder,
  Random,
  MinDegree,
  MinFill,
  Explicit,
};

inline bool usesOnlineElimination(OrderingPolicy Policy) {
  return Policy != OrderingPolicy::Default &&
         Policy != OrderingPolicy::CostAware;
}

struct OrderPolicyOptions final {
  // Untuned engineering defaults, not experimentally calibrated paper values.
  double StructuralCap = 64.0;
  double ExpressionCap = 4096.0;
  double StarCap = 256.0;
  // 0 requests exact counts; a positive value bounds each metadata traversal.
  std::size_t DAGSizeCap = 1024;
  bool Incremental = true;
  bool RecordTrace = false;
  bool MeasureLiveNodes = false;
  // Opt-in common sparse engine for baseline/order-only comparisons. Without
  // this flag Default and legacy CostAware keep their historical algorithms.
  bool UseSparseElimination = false;
  std::uint64_t RandomSeed = 0;
  // Local vertex indices; validated as a complete permutation for Explicit.
  std::vector<std::size_t> ExplicitOrder;
  // Optional read-only snapshot: true when the semantic star of this operand
  // (Expr*, NOT a syntactic Star allocation) is already memoized. The snapshot
  // must remain unchanged during a solve so local dirty marking stays valid.
  // No callback denotes an ordinary uncached interpretation. It does NOT
  // disable iteration exposure or change the selected strategy.
  std::function<bool(const void *)> IsStarResultCached;
};

struct OrderSignals final {
  double structural = 0.0;
  double expression = 0.0;
  double star = 0.0;
  double fill = 0.0;
};

struct CandidateScoreSample final {
  std::size_t region = 0;
  std::size_t step = 0;
  std::size_t node = 0;
  std::size_t version = 0;
  OrderSignals signals;
  double score = 0.0;
};

struct EliminationStepTrace final {
  std::size_t region = 0;
  std::size_t step = 0;
  std::size_t node = 0;
  OrderSignals signals;
  double score = 0.0;
  std::size_t allocated_nodes = 0;
  std::size_t allocated_stars = 0;
  std::size_t live_nodes = 0;
  std::size_t live_edges = 0;
  std::size_t active_nodes = 0;
  std::size_t active_edges = 0;
  std::size_t bypasses = 0;
};

struct OrderingDiagnostics final {
  std::size_t regions = 0;
  std::size_t selected_nodes = 0;
  std::size_t score_refreshes = 0;
  std::size_t stale_heap_entries = 0;
  std::size_t heap_compactions = 0;
  std::size_t allocated_nodes = 0;
  std::size_t initial_allocated_nodes = 0;
  std::size_t elimination_allocated_nodes = 0;
  std::size_t backsubstitution_allocated_nodes = 0;
  std::size_t boundary_allocated_nodes = 0;
  std::size_t allocated_stars = 0;
  std::size_t bypasses = 0;
  std::size_t peak_live_nodes = 0;
  std::size_t peak_live_edges = 0;
  std::size_t peak_active_nodes = 0;
  std::size_t peak_active_edges = 0;
  std::uint64_t metadata_time_ns = 0;
  std::uint64_t scoring_time_ns = 0;
  std::uint64_t heap_time_ns = 0;
  // Inclusive selector work; scoring/heap times are submetrics, not additive.
  std::uint64_t selection_time_ns = 0;
  std::vector<CandidateScoreSample> score_updates;
  std::vector<EliminationStepTrace> trace;

  void append(const OrderingDiagnostics &Other) {
    for (auto Sample : Other.score_updates) {
      Sample.region += regions;
      score_updates.push_back(std::move(Sample));
    }
    for (auto Step : Other.trace) {
      Step.region += regions;
      trace.push_back(std::move(Step));
    }
    regions += Other.regions;
    selected_nodes += Other.selected_nodes;
    score_refreshes += Other.score_refreshes;
    stale_heap_entries += Other.stale_heap_entries;
    heap_compactions += Other.heap_compactions;
    allocated_nodes += Other.allocated_nodes;
    initial_allocated_nodes += Other.initial_allocated_nodes;
    elimination_allocated_nodes += Other.elimination_allocated_nodes;
    backsubstitution_allocated_nodes += Other.backsubstitution_allocated_nodes;
    boundary_allocated_nodes += Other.boundary_allocated_nodes;
    allocated_stars += Other.allocated_stars;
    bypasses += Other.bypasses;
    peak_live_nodes = std::max(peak_live_nodes, Other.peak_live_nodes);
    peak_live_edges = std::max(peak_live_edges, Other.peak_live_edges);
    peak_active_nodes = std::max(peak_active_nodes, Other.peak_active_nodes);
    peak_active_edges = std::max(peak_active_edges, Other.peak_active_edges);
    metadata_time_ns += Other.metadata_time_ns;
    scoring_time_ns += Other.scoring_time_ns;
    heap_time_ns += Other.heap_time_ns;
    selection_time_ns += Other.selection_time_ns;
  }
};

enum class OnNonConvergentStar {
  // Abort solve with NonConvergentStar status.
  Fail,
  // Return the last iterand at the iteration bound.
  ReturnLast,
  // Return domain bottom at the iteration bound.
  ReturnIdentity,
};

enum class SolveStatus {
  Ok,
  FallbackToState,
  NonConvergentStar,
  InvalidProblem,
};

enum class FallbackReason {
  None,
  ADTRejected,
  InvalidProblem,
};

// The first structural precondition that rejected an ADT solve.  Keeping this
// separate from FallbackReason preserves the coarse API while making fallback
// hotspots actionable when profiling real CFGs.
enum class ADTRejectionReason {
  None,
  EmptyTopologicalOrder,
  DisconnectedFromEntry,
  NonBackEdgeCycle,
  EntryNotFirst,
  MissingTopologicalNode,
  InvalidImmediateDominator,
  ADTConstructionFailed,
  MissingADTLeaf,
  EdgeClassificationFailed,
  ForwardEdgeMissesIntervalEntry,
  BackEdgeMissesIntervalEntry,
};

struct SolveDiagnostics final {
  bool used_adt = false;
  EliminationMethod requested_method = EliminationMethod::StateElimination;
  EliminationMethod executed_method = EliminationMethod::StateElimination;
  FallbackReason fallback_reason = FallbackReason::None;
  ADTRejectionReason adt_rejection_reason = ADTRejectionReason::None;
  std::size_t star_iterations_total = 0;
  bool max_star_hit = false;
  // A client removed requested EAN laws that its transfer semantics do not
  // satisfy.
  bool ean_laws_restricted = false;
  // Peak unique path-expression DAG nodes reachable from the WHOLE elimination
  // matrix at any point during construction (state elimination only). Populated
  // only when EliminationOptions::MeasurePeakNodes is set; 0 otherwise. This is
  // the paper's RQ3 "peak construction nodes" metric.
  std::size_t peak_matrix_nodes = 0;
  // Stage timings (microseconds), recorded by the state-elimination path:
  //   generation    = build matrix + eliminate intermediates (raw DAG),
  //   normalization = EAN optimization pass (0 unless EnableEAN),
  //   interpretation = evaluating summaries into client facts.
  // These fill the paper's Table VII stage columns.
  std::size_t gen_time_us = 0;
  std::size_t norm_time_us = 0;
  std::size_t interp_time_us = 0;
  // Time inside semantic Star evaluation; nested intervals are counted once.
  std::uint64_t semantic_star_time_ns = 0;
  OrderingDiagnostics ordering;
  std::size_t procedure_context_count = 0;
  std::size_t procedure_summary_builds = 0;
  std::size_t procedure_summary_reuses = 0;
};

struct EliminationOptions final {
  EliminationMethod Method = EliminationMethod::StateElimination;
  // Pivot-order policy for the state-elimination engine. Default preserves the
  // historical baseline order; CostAware selects the paper's "Order" policy.
  OrderingPolicy Ordering = OrderingPolicy::Default;
  OrderPolicyOptions Order = {};
  OnNonConvergentStar NonConvergentStarPolicy = OnNonConvergentStar::Fail;
  // 0 means "use Problem.maxStarIterations()".
  std::size_t MaxStarIterations = 0;
  // Reserved for future conditional collection. Diagnostics are currently
  // recorded unconditionally by the solver and attached to result metadata.
  bool RecordDiagnostics = true;

  // Opt-in RQ3 instrumentation: when set, the state-elimination engine records
  // SolveDiagnostics::peak_matrix_nodes (peak unique DAG nodes across the whole
  // matrix during construction). Default off — adds a per-pivot reachability
  // scan, so it is enabled only by the evaluation harness.
  bool MeasurePeakNodes = false;

  // EAN (Equality-saturation Algebraic Normalizer) post-optimization. When
  // enabled, the solver runs EAN on the batch of path-expression summaries
  // before interpreting them (see SolverContext::applyEAN). Default off, so the
  // baseline "Default" configuration is unchanged. The default law profile is
  // universally safe (left distributivity only); distributive clients may set a
  // richer profile (RightDistributive/Sliding/...) per their algebra.
  bool EnableEAN = false;
  ean::LawProfile EANLaws = ean::LawProfile::safeMinimal();
  ean::CostModel EANCost = ean::CostModel::uniform();
  ean::Budget EANBudget = ean::Budget::unbounded();
  ean::ExtractOptions EANExtract = {};
  // Greedy post-pass (paper's "Greedy" config): deterministic one-pass prefix
  // factorization, no e-graph, no retained alternatives. Mutually exclusive
  // with EnableEAN (EAN takes precedence if both set). Default off.
  bool EnableGreedy = false;
  // Invocation gate: run EAN only on batches whose raw unique-node count is at
  // least this (0 = always run). Copied into EANExtract.gateMinNodes at use.
  std::size_t EANMinNodes = 0;
  // Monotone guard: EAN returns the input if its output has more unique nodes
  // (never degrade an already-compact input). Copied into EANExtract at use.
  bool EANMonotone = false;
  // Interpret each summary this many times (>=1). Amortization knob for RQ2:
  // reveals the per-query interpretation cost so EAN's cheaper IR can be
  // weighed against its one-time normalization cost. Does not change results.
  std::size_t InterpRepeat = 1;

  // Memoizing (transformer-composition) interpretation. When set, the generic
  // engines SKIP the tree-walking interpreter — they still build/optimize the
  // path-expression ExprTo batch, but leave IN facts for the CLIENT to fill via
  // a memoizing interpreter that evaluates each unique DAG node once (cost ∝
  // unique nodes instead of the expanded tree). This only makes sense for a
  // relational client whose fact is a transformer and whose initialFact is the
  // compositional unit (e.g. the affine-equalities client); other clients must
  // not set it (their IN facts would be left empty). Default off.
  bool InterpMemo = false;
  // Memoize (expression, input fact), not just expression, to preserve guarded
  // and other non-distributive transfer semantics. Reset at each interpretation
  // epoch when external call/return facts may have changed.
  bool MemoizeInterpretation = false;
  std::size_t InterpretationMemoBudget = 4096;
};

inline EliminationOptions defaultContextOptions() {
  EliminationOptions Options;
  Options.Method = EliminationMethod::ADTSimple;
  return Options;
}

// EAN/Greedy post-optimization configuration for the interprocedural
// path-summary solver (ForwardInterSummarySolver). Mirrors the EAN-relevant
// subset of EliminationOptions so the intra and inter stories share identical
// knobs and defaults. All fields default to a no-op pass (EnableEAN and
// EnableGreedy both false), so the interprocedural baseline is unchanged.
//
// Soundness note: the interprocedural summary interpreter composes atoms the
// same way as the intra interpreter (Concat = sequential apply, Union = merge),
// so left distributivity holds unconditionally and the default safe-minimal law
// profile preserves every client's results. Distributive clients may opt into a
// richer profile per their algebra.
struct InterEANOptions final {
  // Run EAN on the batch of context summaries before interpreting them.
  bool EnableEAN = false;
  ean::LawProfile EANLaws = ean::LawProfile::safeMinimal();
  ean::CostModel EANCost = ean::CostModel::uniform();
  ean::Budget EANBudget = ean::Budget::unbounded();
  ean::ExtractOptions EANExtract = {};
  // Greedy post-pass (paper's "Greedy" config). Mutually exclusive with
  // EnableEAN (EAN takes precedence if both set). Default off.
  bool EnableGreedy = false;
  // Invocation gate: run EAN only when the batch's raw unique-node count is at
  // least this (0 = always). Copied into EANExtract.gateMinNodes at use.
  std::size_t EANMinNodes = 0;
  // Monotone guard: return the input batch if EAN's output has more unique
  // nodes. Copied into EANExtract.monotoneGuard at use.
  bool EANMonotone = false;
  // Interpret each summary this many times (>=1). Amortization knob for RQ2.
  std::size_t InterpRepeat = 1;
};

} // namespace elimination

