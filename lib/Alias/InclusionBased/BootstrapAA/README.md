# BootstrapAA

A faithful, standalone implementation of Vineet Kahlon's PLDI 2008 bootstrapping
pointer alias analysis for Lotus, with an LLVM 14 adapter, field-sensitive
struct/array flattening, hierarchy-ordered FSCI dovetailing, guarded backward
function summaries, optional ROBDD path sensitivity, and context-sensitive
interprocedural points-to and alias queries. The public C++ namespace is
`lotus::bootstrap`.

## Relationship to the bootstrapping paper

The design faithfully implements Vineet Kahlon, **Bootstrapping: A Technique for
Scalable Flow and Context-Sensitive Pointer Alias Analysis**, PLDI 2008,
pp. 249–259, while retaining the earlier forward input-state tabulation engine
as an independent reference backend (`SolverMode::InputStateTabulation`) for
differential validation.

| Paper mechanism | Implementation |
| --- | --- |
| Steensgaard partitioning, §2.1 | Direct target unification (`Preprocessing/Steensgaard.cpp`) with per-node nullability (avoiding a single global `NULL` unification bridge), unification-based indirect call resolution, field-sensitive `Field`/`MemCopy`/`Havoc` propagation, Kosaraju SCC cycle collapse, and retained `SteensgaardHierarchy` DAG/depth/cyclic metadata. Distinct partitions are preserved even when a `top` pointer (such as `main`'s `argv`) is present. |
| Optional Das (2000) one-level-flow cascade, §4 | Optional intermediate `oneLevelFlow()` refinement stage (`Preprocessing/OneLevelFlow.cpp`, `Options::enable_one_level_flow`, `--one-level-flow`) bridging Steensgaard partitions and sparse Andersen refinement. |
| Relevant variables & statements (`V_P`, `St_P`), Algorithm 1 | Worklist-based backward closure (`Preprocessing/RelevantSlice.cpp`) computing both relevant values/objects (`V_P`) and the explicit relevant statement slice (`St_P`, `Slice::inst_list`, `Slice::relevant_functions`) using hierarchy ancestors and cyclic components without pulling unrelated indirect calls into every cluster. |
| Thresholded sparse Andersen refinement, §2.1 & Theorem 7 | Sparse `St_P`-restricted inclusion solver (`Preprocessing/AndersenRefinement.cpp`) triggered on partitions exceeding `andersen_threshold` (default 60), forming an overlapping inverse points-to cluster cover (Theorem 7) with deterministic adaptive threshold tuning and size/work guards. |
| Guarded summary tuples, Definition 8 | Interned 4-atom guard language (`Solver/GuardedSummaryState.cpp`: `PointsTo`, `NotPointsTo`, `Alias`, `NotAlias` with call-site suffixes for context qualification), non-loop contradiction pruning, subsumption/back-subsumption, Top-absorption, and sound budget widening (`max_guard_atoms`, `max_guards_per_state`). |
| Top-down Steensgaard-depth dovetailing, Algorithm 2 | `GuardedSummarySolver::runDovetailing()` (`Solver/GuardedSummaryEngine.cpp`) interleaves summary computation and FSCI points-to resolution in increasing Steensgaard hierarchy depth, with optional literal eager per-depth summary precomputation in reverse-topological SCC order (`Options::eager_dovetail_summaries`, `--eager-dovetail`, Algorithm 2 lines 2 & 8). Cyclic depths run a monotone Pass-1 least fixpoint (with dynamic extra-key discovery and sound `m_upper` cap fallback) followed by a Pass-2 precision-tightening pass against the converged Pass-1 sound sets, and a final flow-sensitive indirect-call reachability pass tightens FSCI reachability (`pointsToAllContexts`/`aliasesAllContexts`). |
| Backward tuple transfer, Algorithm 4 | `transferBackward()` (`Solver/GuardedSummaryTransfer.cpp`) filters statements against `St_P` (Algorithm 4 line 2), transfers symbolic expressions (`Val`, `Mem`, `Deref` (`*s`), `Addr`, `Null`, `Top`) across all opcodes and simultaneous incoming PHI assignments, emitting `s -> r` / `s -/-> r` for direct-target assignments (lines 14–17), `u -> q` / `u -/-> q` for `Mem` (lines 24–26), and `s ~ u` / `s !~ u` for indirect `*s` vs `*u` (lines 32–34). |
| Reverse-topological SCC summary computation, Algorithm 5 | `computeSummary()` (`Solver/GuardedSummaryEngine.cpp`) builds summaries bottom-up across the call-graph SCC condensation, runs a deduplicated waiter fixpoint inside recursive SCCs, uses straight-line intra-block fast backward walks + exact intra-block SSA canonicalization, and skips functions whose `St_P`-restricted modification set (`m_func_modifies`) cannot touch the tracked location (`skipped_functions`). |
| FSCI & FSCS points-to / direct alias queries, Algorithm 3 & §3 | `computeFsci()` / `computeFscs()` compute backward entry anchors `A` (with Algorithm 3's `Retain` fast-path and flow-sensitive indirect-call reachability); `propagateForwardAnchorsFsci()` / `propagateForwardAnchorsFscs()` implement Algorithm 3 Step 2 forward summary propagation from `A` to `Q` for `aliases()` / `aliasesAllContexts()`. |
| Optional ROBDD path sensitivity, §3 | Dependency-free canonical ROBDD manager (`Support/Bdd.h`, `Support/Bdd.cpp`) tracking branch condition formulas `con_b` across intraprocedural and interprocedural paths (both downward into summaries and upward across callers in `computeFsci()`/`computeFscs()` via `path_sensitive_interprocedural`), with cross-function LLVM predicate canonicalization for shared formal arguments and non-escaping local globals, sound existential quantification on condition redefinitions/kills (`defines_condition`, `kills_conditions`, `entry_kills_conditions`, `global_conditions`), and node-budget widening (`max_bdd_nodes`). |
| Field sensitivity, heap objects, function pointers, Remark 1 | LLVM 14 adapter (`LLVM/BootstrapAA.cpp`) recursively flattens structs and folds arrays via `ArrayRegion` metadata, models constant aggregate global initializers, infers heap struct layouts from unique allocation-site bitcasts, models `Opcode::Field` GEPs and `Opcode::MemCopy` transfers, and performs byte-range `Havoc` invalidation on non-pointer stores/atomics. |
| Parallel per-cluster execution, §4 | Missing clusters required by a query (or all clusters under `precomputeAll()`) are dispatched largest-first across a bounded `std::thread` worker pool with isolated solver state and deterministic statistics aggregation (`Engine.cpp`). |

### Reference backend

Setting `Options::solver_mode = SolverMode::InputStateTabulation` (or passing
`--solver=tabulation` to `lotus-alias-bootstrap`) selects the forward
input-state-tabulated reference solver (`Solver/TabulationSolver.cpp`). Keeping
both backends behind the shared `ClusterSolver` interface enables continuous
differential testing across generated programs and real LLVM IR benchmarks.

## Source layout and targets

All headers live under `include/Alias/InclusionBased/BootstrapAA/` and all
implementations under `lib/Alias/InclusionBased/BootstrapAA/` (no headers in
`lib/`):

```text
include/Alias/InclusionBased/BootstrapAA/
├── BootstrapAA.h                     # Public LLVM 14 API
├── Engine.h                          # Public LLVM-independent IR, lattice, options & Analysis API
├── IR/
│   └── ProgramIndex.h                # ProgramIndex, CallGraphInfo, ReachabilityInfo, Targets
├── Preprocessing/
│   ├── Partitioning.h                # Steensgaard (§2.1), One-Level Flow (§4), Andersen (Thm. 7)
│   └── RelevantSlice.h               # Algorithm 1 Slice (V_P, St_P), relevant()
├── Solver/
│   ├── ClusterSolver.h               # Shared per-cluster solver interface & factories
│   └── GuardedSummarySolver.h        # Def. 8 Guard/Expr/SummaryKey & GuardedSummarySolver
└── Support/
    └── Bdd.h                         # Canonical ROBDD manager (§3 path sensitivity)

lib/Alias/InclusionBased/BootstrapAA/
├── Engine.cpp                        # Analysis orchestration, adaptive clustering, parallel pool
├── IR/
│   ├── PointsToSet.cpp               # Sparse word-bitset lattice & hierarchy reachability
│   ├── Program.cpp                   # Program builder, field/array resolution, validation
│   └── ProgramIndex.cpp              # Program indices, Kosaraju call-graph SCCs, reachability
├── Preprocessing/
│   ├── Steensgaard.cpp               # §2.1 Steensgaard unification & cycle-collapsed hierarchy
│   ├── OneLevelFlow.cpp              # §4 Das (2000) one-level-flow cascade
│   ├── RelevantSlice.cpp             # Algorithm 1 worklist (V_P, St_P)
│   └── AndersenRefinement.cpp        # Sparse St_P Andersen refinement
├── Solver/
│   ├── GuardedSummaryState.cpp       # Def. 8 guards, offsets, St_P mod-ref, FSCI/guard eval
│   ├── GuardedSummaryTransfer.cpp    # Algorithm 4 backward transfer & PHI substitution
│   ├── GuardedSummaryEngine.cpp      # Alg. 5 SCC summaries, Alg. 2 dovetailing, Alg. 3 A+Q
│   └── TabulationSolver.cpp          # Reference forward input-state tabulation oracle
├── Support/
│   └── Bdd.cpp                       # Canonical ROBDD manager
└── LLVM/
    └── BootstrapAA.cpp               # Remark 1 LLVM 14 lowering, flattening & adapter API
```

Additional files:
- `tools/alias/lotus-alias-bootstrap.cpp`: command-line driver.
- `tests/unit/Alias/InclusionBased/BootstrapAA/`: GTest engine (`EngineTest.cpp`)
  and LLVM adapter (`BootstrapAATest.cpp`) suites.

`CanaryBootstrapCore` is a dependency-free C++17 library (linking only
`Threads::Threads`). `CanaryBootstrapAA` adds LLVM Core/Support.
`lotus-alias-bootstrap` additionally links LLVM IRReader. BootstrapAA is not
wired into `AliasAnalysisWrapper` because its queries require a program point
and optionally an exact call-site context.

## Build in Lotus

```sh
cmake -S . -B build -DLOTUS_BUILD_TESTS=ON \
  -DLLVM_BUILD_PATH=/path/to/llvm-14/lib/cmake/llvm
cmake --build build --target CanaryBootstrapAA lotus-alias-bootstrap \
  alias_tests
ctest --test-dir build -R 'Bootstrap|bootstrap_aa' --output-on-failure
```

## Program-point, context, and alias queries

```cpp
#include "Alias/InclusionBased/BootstrapAA/BootstrapAA.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace lotus::bootstrap;

// M must remain alive and unchanged. The default entry is a defined main.
Options opts;
opts.solver_mode = SolverMode::GuardedSummary; // default (PLDI '08 Algorithms 2-5)
opts.path_sensitive = false;                   // set true for ROBDD path sensitivity
opts.enable_one_level_flow = false;            // set true for Das one-level-flow stage
BootstrapAA analysis(M, nullptr, opts);

// value is an LLVM scalar pointer value in M; location is an instruction in M.
// Before means immediately before location; After includes location's effect.
QueryResult result = analysis.pointsTo(value, location, {}, Point::Before);

// Query inside a callee reached by an exact sequence of call instructions,
// ordered from the entry activation to the queried activation.
BootstrapAA::CallContext path{&outer_call, &inner_call};
QueryResult contextual = analysis.pointsTo(callee_value, callee_location, path);

// Explicitly context-insensitive (FSCI) query across all reachable activations.
QueryResult joined = analysis.pointsToAllContexts(callee_value, callee_location);

// Direct alias sets at (location, Point::Before) (Algorithm 3 Step 2).
LLVMAliasResult alias_set = analysis.aliases(value, location);
LLVMAliasResult fsci_aliases = analysis.aliasesAllContexts(value, location);

// Look up a flattened struct field's abstract object ID by byte offset.
Id field_id = analysis.fieldObjectId(alloca_inst, /*byte_offset=*/8);

if (result.status == QueryStatus::ResourceLimit) {
  // result.points_to is top, never an incomplete/under-approximated set.
}
for (Id object : result.points_to.objects()) {
  const llvm::Value *site = analysis.allocationSite(object);
  // UNKNOWN and NULL_OBJECT have no LLVM allocation site (site == nullptr).
  (void)site;
}

const SteensgaardHierarchy &hierarchy = analysis.hierarchy();
(void)hierarchy.depth;

// Optional: eagerly build every cluster solver using configured parallelism.
analysis.precomputeAll();
```

An empty call context means **the entry activation**, not "any caller." Contexts
contain call-site identities, so distinct calls to the same function remain
distinguishable and recursion is represented by repeating call sites. A
well-formed context that cannot reach the site returns `Unreachable`. At a PHI
block entry, all incoming PHIs are assigned simultaneously on the incoming CFG
edge.

| Status | Meaning |
| --- | --- |
| `Complete` | The modeled analysis reached its fixed point (results may still contain conservative `top`). |
| `Unreachable` | No modeled activation at the requested context/point. The returned set is empty. |
| `ResourceLimit` | Context or step budget exhausted. The returned set is `top`. |

## Execution and memory model

The model is sequential and closed-world, starting at the selected entry after
static initialization. Root pointer parameters and unknown external objects are
`top`. Scalar, struct, and array global initializers (`ConstantStruct`,
`ConstantArray`, `ConstantAggregateZero`) are modeled field-sensitively; the
presence of `llvm.global_ctors` conservatively initializes global storage to
`top`.

Struct and array types (up to 64 flattened fields) are recursively flattened
into distinct offset-indexed abstract objects, with array elements folded via
`ArrayRegion` stride metadata so variable-index GEPs within an array of structs
preserve sibling struct fields. Scalar pointer globals and non-recursive,
entry-block, single-cell static allocas (and their non-array struct pointer
fields) support strong updates; heap allocations, array elements, loop
allocations, and recursive-frame allocations are non-singleton and always
receive weak updates.

Fresh `alloca` and allocator (`malloc`, `calloc`, `aligned_alloc`, `valloc`,
`memalign`, scalar/array `operator new`) sites start with empty initial points-to
sets (matching fresh uninitialized/zeroed storage without collapsing all
allocations into `top` during Steensgaard preprocessing); `byval` copies,
`realloc` old/fresh alternatives, and opaque external writes remain conservative.
When an untyped heap allocation is uniquely bitcast to a struct pointer type,
the adapter flattens that heap site's struct fields; otherwise it models a
pointer-sized cell. Non-pointer stores and memory intrinsics invalidate only
overlapping byte ranges via `Opcode::Havoc`, while `llvm.memcpy`/`llvm.memmove`
lower to field-preserving `Opcode::MemCopy` (with zero-length intrinsics kept as
no-ops). Non-default address-space nulls, `null_pointer_is_valid`, non-inbounds
pointer arithmetic, `inttoptr`, `addrspacecast`, `freeze`, vector pointer
extracts, inline assembly, and exceptional call alternatives (`invoke`/`callbr`)
are modeled conservatively.

Concurrency, signal handlers, arbitrary reentrant external callbacks, dynamic
loading, and sub-byte/unaligned cross-field integer-pointer reinterpretation
outside the modeled IR contract are not modeled.

## Resource budgets and widening

- Default budgets per cluster solver: `max_contexts = 4096`, `max_steps =
  1000000`. Exhausting either returns `QueryStatus::ResourceLimit` with
  `PointsToSet::top()` (never a partial under-approximation).
- Guard widening: `max_guard_atoms = 16` and `max_guards_per_state = 16` soundly
  widen complex guard conjunctions to `GUARD_TRUE` instead of dropping paths.
- ROBDD budget: `max_bdd_nodes = 16384` soundly widens ROBDD operations to
  `TRUE` if the node table reaches capacity.
- Cached cluster solvers are reused across queries on an `Analysis`/`BootstrapAA`
  instance; concurrent queries on the same instance require external
  synchronization (internal cluster construction parallelism is thread-safe).

## Driver

```sh
./build/bin/lotus-alias-bootstrap input.bc
./build/bin/lotus-alias-bootstrap --all-contexts \
  --solver=guarded --one-level-flow --path-sensitive \
  --andersen-threshold=60 --threads=8 input.bc
./build/bin/lotus-alias-bootstrap --print-aliases input.bc
```

Key CLI flags:
- `--solver=guarded|tabulation`: select the PLDI '08 guarded-summary backend
  (default) or the input-state tabulation reference backend.
- `--all-contexts`: query FSCI points-to/aliases across all functions (enables
  eager cluster precomputation).
- `--print-aliases`: print direct alias sets at each pointer-producing
  instruction instead of points-to sets.
- `--one-level-flow`: enable the optional Das (2000) one-level-flow cascade
  stage before Andersen refinement.
- `--path-sensitive` / `--path-sensitive-interproc`: enable intraprocedural or
  interprocedural (downward + upward) ROBDD path sensitivity.
- `--eager-dovetail`: eagerly precompute function summaries per Steensgaard
  depth in reverse-topological SCC order (literal Algorithm 2 lines 2 & 8).
- `--detailed-stats`: print partition/cluster size distributions and per-cluster
  solve times in addition to paper-oriented aggregate counters (`dovetail-levels`,
  `fsci-queries`, `summary-tuples`, `guard-atoms`, `guards-pruned`,
  `guards-widened`, `skipped-functions`, `path-prunes`, `bdd-nodes`,
  `one-level-flow-runs`, etc.) on `stderr`.
- `--adaptive-threshold=false`, `--parallel-clusters=false`,
  `--max-andersen-partition=N`, `--max-andersen-work=N`,
  `--precompute-clusters`, `--max-contexts=N`, `--max-steps=N`, `--threads=N`.

Exit codes: `0` on completion, `1` on CLI/IR errors, `2` if any printed query
required a `ResourceLimit` fallback.

## Validation

- **Unit & regression tests**: 70/70 CTest cases pass (45 `BootstrapEngineTest`
  tests covering Figures 2–5, Definition 8, Algorithms 1–5, FSCS guards,
  `*s` alias & direct-target guards, multi-depth & eager Algorithm 2
  dovetailing, flow-sensitive indirect-call reachability for FSCI, Algorithm 3
  backward-`A` + forward-`Q` interprocedural aliases, ROBDD
  intraprocedural/interprocedural downward & upward/loop path sensitivity,
  one-level flow, cyclic linked lists, and 200-program slicing + 150-program
  cross-backend differential fuzzing; 24 `BootstrapAATest` LLVM 14 tests
  covering struct/array flattening, `MemCopy`, partial byte overwrites, heap
  struct inference, function pointers in structs, `main(argv)` partitioning,
  LLVM intraprocedural & interprocedural ROBDD branch pruning and argument/global
  kill soundness, `aliases()`, and `fieldObjectId()`; plus
  `bootstrap_aa_cli_smoke`).
- **Real LLVM IR differential audit**: Evaluated `lotus-alias-bootstrap
  --all-contexts` across all 62 regression IR benchmarks under
  `tests/regress/Alias/**/*.ll` (including `PTA/*.ll`, `spec-equake.ll`,
  `spec-mesa.ll`, `spec-parser.ll`, and `FlowSensitive/differential/*.ll`):
  - `guarded` vs `tabulation`: **62/62 exit 0 (0 crashes, 0 fallbacks)**,
    **0 unsound results**, **60/62 byte-for-byte identical** (only
    `PTA/global-array.ll` [3 coarser] and `PTA/spec-mesa.ll` [8 coarser] are
    slightly coarser sound supersets).
  - `--eager-dovetail` vs default `guarded`: **62/62 byte-for-byte identical**.
  - `--one-level-flow`, `--path-sensitive`, and
    `--path-sensitive --path-sensitive-interproc`: **62/62 exit 0** with 0
    unsound results.
  - `--print-aliases` across all 62 files: **61/62 exit 0** at default
    `max_contexts=4096` (`spec-equake.ll` triggers a sound `ResourceLimit`
    fallback under all-pairs `--all-contexts --print-aliases` on its 292-value
    cluster).
- **Strict warnings & sanitizers**:
  - `CanaryBootstrapCore` + `EngineTest.cpp` (all 45 tests) compile cleanly with
    `clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror` and pass under both
    `-fsanitize=address,undefined` (ASan/UBSan) and `-fsanitize=thread` (TSan).
  - `LLVM/BootstrapAA.cpp`, `lotus-alias-bootstrap.cpp`, and `BootstrapAATest.cpp`
    compile with zero warnings under `-std=c++17 -Wall -Wextra -Wpedantic -Werror`
    against LLVM 14.
