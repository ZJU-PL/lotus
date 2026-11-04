# Algebraic Program Analysis (the Elimination Method for Dataflow Analysis)

This directory provides **elimination-based** dataflow solvers that compute
solutions by progressively summarizing paths, conceptually similar to state
elimination in automata / Gaussian elimination over path expressions. The
current implementation includes both intraprocedural elimination and a
call-string-based interprocedural layer that reuses the intraprocedural solver
per procedure/context.

## Positioning: what "APA" means here

The directory is named `APA` because it implements an **algebraic, path-expression-based**
style of analysis, including paper-style ADT elimination variants on reducible flowgraphs.

However, this is **not** a full general-purpose implementation of all
"Algebraic Program Analysis" machinery. In Lotus today, this component should
be read as:

- an APA-inspired elimination engine with strong **intraprocedural** support,
- a lightweight **call-string-sensitive interprocedural** extension for selected
  LLVM analyses,
- specialized to LLVM CFG / ICFG clients,
- aimed at forward MOP-style dataflow clients such as reachability, constant
  propagation, and reaching definitions.

For broader interprocedural formulations, see other frameworks in this repository
(e.g., IFDS/IDE, WPDS, and NPA modules).

## Public header layout

The public API lives under `include/Dataflow/APA/`:

```text
include/Dataflow/APA/
├── APA.h                          # Canonical umbrella for the framework
├── Core/                          # Generic problem, path-expression, options, results
├── Solver/                        # Solver facades and concrete elimination engines
├── LLVM/                          # LLVM problem and CFG integration
├── Domains/                       # Abstract values and lattice operations
├── Analyses/Intra/                # Intraprocedural LLVM transfer semantics
├── Analyses/Inter/                # Interprocedural LLVM transfer semantics
└── Passes/                        # Legacy-pass wrappers
```

The layout shown above is the supported public header structure.

The solver tree is grouped by responsibility:

```text
Solver/
├── Intra/          # IntraSolver, run state, post-processing, ADT components
├── Inter/          # Context-cached and expanded-ICFG solvers; Modular/ components
├── Equations/      # Equation graph, options, results, and SCC scheduling
├── Elimination/    # Shared dense/sparse kernels and reachable-DAG metrics
├── Interpretation/ # Input-sensitive interpretation and semantic memoization
├── Graph/          # Generic SCC algorithm
└── Ordering/
    ├── Policies/   # Structural, ExpressionAware, StarRisk, Hybrid; Baselines
    ├── Policy.h    # Policy registration, requirements, and validation
    ├── Signals.h   # Read-only local cost signals and cached metadata lookup
    ├── Selector.h  # Dirty candidates and versioned heap; no policy formulas
    └── StructuralModel.h # Graph-only ordering simulation (comparison baseline)
```

The public and implementation trees are intentionally not exact mirrors.
`include/Dataflow/APA/` also contains template implementations that must remain
visible to clients, while `lib/Dataflow/APA/` contains only separately compiled
non-template implementations.

### Quick include guide

- Framework umbrella: `#include "Dataflow/APA/APA.h"`
- Minimal intraprocedural surface: `Core/Problem.h`, `Core/Result.h`,
  `Solver/Intra/IntraSolver.h`, `LLVM/ForwardProblem.h`
- Minimal interprocedural surface: `Core/InterProblem.h`, `Core/InterResult.h`,
  `Solver/Inter/ContextSolver.h`
- Abstract domains: `Domains/*.h`
- LLVM clients: `Analyses/Intra/*.h` and `Analyses/Inter/*.h`
- Passes: `#include "Dataflow/APA/Passes/EliminationPasses.h"`
- Internal engine headers: `Solver/Intra/Context.h` and the concrete
  `*Solver.h` files are solver internals; downstream clients should normally
  include only `Solver/Intra/IntraSolver.h` or `Solver/Inter/ContextSolver.h`.

## References

### Classical Elimination-Based Dataflow Analysis

NOTE: some of them may not use path expressions.

- Static Analysis by Elimination. Pavle Subotic, Andrew E. Santosa,  and
Bernhard Scholz.
- ETAPS’07:  A new elimination-based data flow analysis framework using
annotated decomposition trees. B. Scholz and J. Blieberger.
- TOPLAS'98: A new framework for elimination-based data flow analysis using DJ graphs. V. C. Sreedhar, G. R. Gao, and Y.-F. Lee. 
- CSUR'86: Elimination Algorithms for Data Flow Analysis. Babara Ryder and Marvin Paull.
- JACM'79: Applications of path compression on balanced trees. R. Tarjan.
- JACM'76: Fast and usually linear algorithm for global flow analysis. S. L. Graham and M. Wegman.
- SIAM J. Comput'77: A simple algorithm for global data flow analysis problems. M. S. Hecht and J. D. Ullman.

### Algebraic Program Analysis (Reps & Kincaid, etc.)

- CAV'21: Algebraic Program Analysis (Tutorial)
- POPL'19: Refinement of Path Expressions for Static Analysis. John Cypher, Jason Breck, Zak Kincaid, Thomas Reps.
- PLDI'17: Compositional Recurrence Analysis Revisited
- FMCAD'15: Compositional Recurrence Analysis 


## What it computes

The solver constructs **path expressions** (regular-expression-like ASTs) over edge transfer functions (`Atom`, `Union`, `Concat`, `Star`) and then evaluates those expressions over your abstract domain using `bottom`, `join`, `equal`, and `applyTransfer`.

This corresponds to a **meet-over-all-paths (MOP)** computation. For classic distributive frameworks, MOP equals the standard maximal fixed point (MFP) solution.

## Relation to `Utils/Algorithms/PathExpressions`

Lotus also ships a generic path-expression utility under
`include/Utils/Algorithms/PathExpressions/`. That component computes ordinary
regular expressions over edge labels in arbitrary labeled graphs.

The APA solver is different:

- APA path expressions carry **transfer functions**, not plain labels.
- APA expressions are **evaluated over a dataflow lattice** via
  `applyTransfer`, domain `join`, and `maxStarIterations`.
- The utility path-expression library is for **regex/path summarization** and is
  not a drop-in implementation of the APA solver.

## Layering

- `Core/` is generic and does not depend on LLVM.
- `Solver/` contains both the generic intraprocedural elimination engines and
  the call-string interprocedural worklist solver.
- `LLVM/` maps LLVM CFGs / ICFGs into the generic problem interfaces.
- `Domains/` owns abstract fact types and their `bottom`/`join`/`equal`
  operations.
- `Analyses/Intra/` and `Analyses/Inter/` provide LLVM transfer behavior,
  boundary conditions, and solver entry points. A domain may support either or
  both scopes. Shared inter-analysis fact propagation helpers also live under
  `Analyses/Inter/`.

`Core/AbstractDomain.h` defines APA's engine-specific domain contract. May
domains normally use union as `join`; must domains use reverse-inclusion order,
so intersection is still the domain `join` and the finite universe is
`bottom`. Transfer functions and boundary facts remain problem responsibilities.

APA is therefore a generic elimination framework, not a complete
"analysis generator." Each client analysis combines a reusable domain with
LLVM-specific modeling.

## Client cost model

The standard clients keep representation and LLVM-query overhead small so
solver measurements primarily reflect path-expression construction and
evaluation:

- finite set domains use a shared value-to-ID universe and copy-on-write
  bitmaps;
- constant propagation and sign analysis use indexed, copy-on-write pages;
- gen/kill analyses precompute their transfer masks once per instruction;
- alias, MemorySSA, value-tracking, and instruction-simplification queries that
  do not depend on the incoming fact are cached before solving; and
- constant propagation uses the finite lattice `unknown / constant /
  overdefined`, without gradually expanding `ConstantRange` values in `Star`.

Affine equalities exposes two vocabulary modes. `AllScalars` retains every
reachable integer SSA value for precision-oriented checks. `ObservableSlice`
tracks the backward slice of entry arguments, control conditions, returns, and
defined-callee argument mappings, and can impose a sound tracking budget by
havocing values outside the slice. `lotus-dfa-apa --analysis=inter_affine` uses
the sliced mode with `--affine-max-tracked=32` by default; pass `0` for an
unlimited slice.

These choices are deliberate for APA experiments: replaying an atom should be
a small abstract-domain operation rather than a fresh interpretation of the
LLVM instruction.

## Current gaps / non-goals

- **Interprocedural support is deliberately lightweight**: the current solver is
  call-string based, solves one procedure/context at a time, and does not claim
  parity with the repository's IFDS/IDE, WPDS, or NPA frameworks.
- **Not a universal semiring-equation engine**: it uses path-expression elimination with
  domain-defined `join` plus problem-defined `applyTransfer`, rather than exposing the full range of algebraic
  solver variants used across APA literature.
- **ADT methods are conditional**: `ADTSimple` / `ADTDelayed` require reducible-graph
  assumptions; the solver falls back to `StateElimination` when assumptions do not hold.
- **No claim of complete APA feature parity**: this module does not attempt to cover all
  formulations (e.g., every interprocedural or Newtonian/tensor-product variant).
- **Engineering tradeoff**: path-expression growth can still be substantial on large CFGs;
  this module focuses on practical LLVM analyses rather than full APA
  scalability research coverage.

## Solver methods

### Order-aware state elimination

`EliminationOptions::Ordering` supports four order-aware policies:
`Structural`, `ExpressionAware`, `StarRisk`, and `Hybrid`, plus
`ReversePostOrder`, seeded `Random`, `MinDegree`, `MinFill`, and `Explicit`
baselines. `Default` and `CostAware` are retained as reference policies.

The four order-aware policies use the shared `Solver/Elimination/SparseSolver.h` engine. It removes
each pivot from the live graph and back-substitutes its recorded equation to
recover entry-to-node summaries at **every** program point. Online ordering requires
`StateElimination`; incompatible ADT/ordering requests are rejected rather than
silently changing engines. `PathSummaryEquationOptions`
exposes the same `Ordering` and `Order` settings for both equation directions,
the whole-program forward-summary solver, and the modular per-procedure builder.
The separate interprocedural affine module driver exposes equivalent `ordering`
and `order` fields in `InterAffineEqualitiesOptions`, forwarding them to its
per-procedure solver while keeping ADT as its default.

Policy scores:

- Structural: `|P| * |Q|`, excluding the pivot itself.
- Expression-aware: sum of `size(in) + size(self) + size(out) + 2` over bypasses;
  a zero/identity self-summary contributes zero.
- Star-risk: capped normalized structural cost plus iteration exposure.
- Hybrid: equal-weight capped normalized structural, expression, and iteration costs.

Reachable-node counts are cached beside immutable DAG roots, with a configurable
size cap. Construction warms the cache before scoring; scoring never traverses
the DAG. Iteration exposure is zero for trivial self-summaries or a pre-memoized
semantic star, otherwise the cached self-summary size. A syntactic hash-consed
Star is **not** evidence that semantic iteration has been memoized. Clients may
supply an immutable `Order.IsStarResultCached` snapshot keyed by the **operand**
expression pointer. No callback means an ordinary uncached interpretation:
Star-risk still uses iteration exposure, and Hybrid still uses all three signals.
Strategies always report Structural as a requested term alongside the others; DAG sizes
are always prepared for policies that require them, and missing prepared metadata is
an error.

`Solver/Ordering/Selector.h` maintains dirty candidates and a versioned min-heap.
Only pivot predecessors/successors are refreshed after each elimination. Ties
use a stable reverse-postorder rank, then the input node index. Periodic heap
compaction bounds obsolete entries. `Order.Incremental=false` enables full
rescoring for differential validation and ablation.

Example:

```sh
build/bin/lotus-dfa-apa input.bc --analysis=inter_reachable \
  --ordering=hybrid --measure-peak --order-trace --stdout
```

The CLI uses `--inter-engine=context|expanded|modular` (default: `context`).
The context solver builds expressions once per procedure/call-string pair and
reinterprets them with a fresh input-sensitive memo whenever call/return facts
change. Diagnostics expose pair/build/reuse counts and per-pair node/trace mappings.
`expanded` retains the global instruction/context equation graph; `modular` is
the functional/context-insensitive alternative, currently exposed for reachability.
Other controls are `--order-{struct,expr,star}-cap`, `--order-size-cap`
(0 means exact), `--order-full-rescore`, and `--order-seed`. Explicit permutations use
`--ordering=explicit --order-explicit=0,1,...`; indices are local to the
intraprocedural graph or each cyclic equation SCC, and must be complete and unique.
Invalid normalization caps and permutations are rejected.

For **order-only performance comparisons**, enable `--order-sparse` for every
configuration, including Default/CostAware, so all use the same sparse engine.
The sparse intraprocedural Default uses a fixed pivot permutation;
the sparse equation Default uses ascending local SCC indices. Do not attribute
differences against the dense full-matrix engine solely to ordering.

Diagnostics include initial construction, elimination and back-substitution
allocations, bypass counts, peak reachable live-summary DAG nodes/child edges,
metadata/scoring/heap times, candidate refreshes, and opt-in per-step traces.
Live-summary measurements include transient fill, recorded equations and completed
query summaries, but exclude factory-only roots; they are **not physical RSS**.
Separate `peak_active_*` metrics restrict roots to the current live graph.
Back-substitution allocations are
included in aggregate counts, not attributed to pivot traces. Ranking time is
metadata time plus inclusive selector time; scoring/heap times are submetrics.
Semantic-star time counts nested intervals only once. RSS and whole-analysis
timings remain available in the CLI. Generic equation totals include the graph
factory's input allocations; separate new-allocation deltas distinguish warm solves.

The default normalization caps (64, 4096, 256) and DAG-size cap (1024) are
engineering defaults, **not calibrated evaluation results**. Path languages are
preserved under every order; fact equality additionally requires a
language-invariant client interpretation. Non-distributive/guarded clients and
bounded nonconvergent stars must not be assumed order-independent.

`OnlineOrderTest.cpp` includes worked-score checks, randomized incremental/full
differentials, exact bounded-language comparison against direct graph paths,
and exhaustive four-node permutations for separate allocation/live-DAG oracles.
These validate the mechanism; they do not establish real-program
performance, statistical significance, or empirically calibrated caps.

Three elimination-style solvers are exposed via `elimination::EliminationOptions`:

- `StateElimination` (default): generic **O(n³)** state-elimination over all nodes (Floyd–Warshall-style).
- `ADTSimple`: **paper-style ADT "simple" algorithm** for **reducible** flowgraphs (O(n²) updates).
- `ADTDelayed`: **paper-style ADT "delayed" algorithm** for **reducible** flowgraphs.

`EliminationOptions` also controls non-convergent `Star` behavior:

- `NonConvergentStarPolicy = Fail | ReturnLast | ReturnIdentity`
- `MaxStarIterations` (0 means use `Problem.maxStarIterations()`).

`Solver/Intra/IntraSolver.h` dispatches to `Intra/StateSolver.h`,
`Intra/ADT/SimpleSolver.h`, or `Intra/ADT/DelayedSolver.h`. `Intra/Context.h`
holds run state; `Intra/ADT/{Types,Reducibility,Decomposition}.h` owns structural
construction; `Interpretation/FactInterpreter.h` owns interpretation, and
`Intra/PostProcessing.h` owns EAN/Greedy integration. Equation scheduling uses
the same shared `Elimination/{DenseClosure,SparseSolver}.h` kernels.

For ADT-based methods, you can optionally implement
`elimination::IntraReducibleEliminationProblem`
(dominators + topological order + edge list). If not provided, the solver computes reducible
flowgraph metadata internally and falls back to `StateElimination` when reducibility assumptions fail.

The synthesized reducible view accepts ADT only when all nodes are entry-reachable,
immediate dominators are computable, and the non-back-edge subgraph is acyclic
with entry first in topological order.

## Summary-equation graph solver

`include/Dataflow/APA/Solver/Equations/Solver.h` provides a generic
solver for left-linear path-summary equations:

```text
X_u = base_u U (W_u,v . X_v)
```

Here `X_u` is a summary instance, such as a future `(function, context)` node,
and `W_u,v` is an APA `PathExprFactory` expression. The solver computes SCCs in
the summary-dependency graph, solves SCCs in dependency order, and uses a
state-elimination closure inside cyclic SCCs so
recursive summary dependencies are represented with `Star` expressions rather
than unbounded worklist growth.

This component is intentionally APA-specific: the scheduled objects are
path-expression equations and the output is a closed-form path-expression
summary for each key. It can be used as the algebraic core for a future
interprocedural summary-substitution solver, instead of merely scheduling calls
to an arbitrary intraprocedural analysis.

`PathSummaryEquationSolver` also supports a forward-path mode for equations
where a node summary is extended by outgoing transfer expressions. The forward
interprocedural prototype in
`include/Dataflow/APA/Solver/Inter/ExpandedSolver.h` uses that mode over
instruction/context nodes and labels interprocedural edges with
`InterSummaryTransferAtom` values (`RawNormal`, `CallEntry`, `ReturnExit`, and
`CallToRet`). This gives a real summary-substitution path for forward analyses:
call-entry, return, and bypass effects are represented as path-expression atoms
and recursive context dependencies are closed by SCC-local `Star` expressions.

LLVM client entry points currently include:

- `runInterSummaryElimReachability`
- `runInterSummaryElimAvailableExpressions`
- `runInterSummaryElimConstantPropagation`
- `runInterSummaryElimNonNull`
- `runInterSummaryElimReachingDefinitions`
- `runInterSummaryElimSign`
- `runInterSummaryElimUninitializedVariables`
- `runInterSummaryElimLockset`

These are tested for parity with the existing worklist-style interprocedural
solver on focused forward-analysis cases. Lockset wrapper propagation is also
tested directly because the summary graph can preserve a callee-return fact that
the worklist path currently drops. The generic solver is intentionally
forward-only at this stage; affine equalities remain out of scope for this
backend.

## Interprocedural call-string solver

Interprocedural APA clients are modeled by
`elimination::InterEliminationProblem` and solved by
`elimination::InterEliminationSolver<AnalysisTypesT, K>`. The solver is
context-sensitive via bounded call strings, using
`mono::CallStringCTX<Instruction *, K>` as the context representation.

The solver proceeds by:

- maintaining `IN` / `OUT` facts keyed by `(instruction, call-string context)`,
- computing a boundary fact for one procedure/context from `callFlow` or
  `returnFlow`,
- scheduling work at `(procedure, call-string context)` granularity,
- solving that procedure with ADT-simple elimination, with state elimination as
  the fallback for rejected CFGs, and
- propagating changes between dependent caller and callee procedure contexts.

The solver records explicit call links from each limited callee context back to
the caller contexts that produced it. This is required once the call string is
full: dropping the oldest call site during `push_back` makes the caller context
impossible to reconstruct with `pop_back`. The same links implement `K = 0` by
merging all callers into the empty context without treating callees as roots.

Clients provide four interprocedural hooks on top of the normal-flow lattice:

- `callFlow(CallSite, Callee, In)` to build the callee-entry fact,
- `returnFlow(CallSite, Callee, ExitStmt, RetSite, In)` to map callee exit facts
  back to the caller,
- `callToRetFlow(CallSite, RetSite, Callees, In)` for the bypass edge,
- `getCalleesOfCallAt(CallSite)` for call resolution.

The default LLVM adapter `LLVMInterEliminationProblem` handles direct calls,
provides a conservative signature-based fallback for indirect calls, and can
optionally warn when indirect-call resolution is missing.

### Context sensitivity

The shipped interprocedural analyses currently use a default call-string bound
of `K = 2`:

- `kDefaultInterElimReachabilityCallStringLength`
- `kDefaultInterElimAvailableExpressionsCallStringLength`
- `kDefaultInterElimConstantPropagationCallStringLength`
- `kDefaultInterElimNonNullCallStringLength`
- `kDefaultInterElimUninitializedVariablesCallStringLength`
- `kDefaultInterElimReachingDefinitionsCallStringLength`
- `kDefaultInterElimLocksetCallStringLength`
- `kDefaultInterElimSignCallStringLength`

`K = 0` is also supported by the generic solver and degenerates to
context-insensitive return propagation.


## Intraprocedural LLVM analyses

We provide a few concrete LLVM IR analyses implemented on top of the
elimination framework. These are intended as practical clients (as in the
paper), and serve as examples for adding additional analyses:

- Reachability (`runIntraElimReachability`)
- Constant propagation (`runIntraElimConstantPropagation`)
- Uninitialized variables (`runIntraElimUninitializedVariables`)
- Reaching definitions (`runIntraElimReachingDefinitions`)
- Available expressions (`runIntraElimAvailableExpressions`)
- Lockset analysis (`runIntraElimLockset`)
- Non-null propagation (`runIntraElimNonNull`)
- Sign analysis (`runIntraElimSign`)
- Affine equalities (`runIntraElimAffineEqualities`)

## Interprocedural LLVM analyses

All nine LLVM analyses also expose interprocedural entry points:

- Reachability (`runInterElimReachability`)
- Available expressions (`runInterElimAvailableExpressions`)
- Constant propagation (`runInterElimConstantPropagation`)
- Non-null propagation (`runInterElimNonNull`)
- Uninitialized variables (`runInterElimUninitializedVariables`)
- Reaching definitions (`runInterElimReachingDefinitions`)
- Lockset analysis (`runInterElimLockset`)
- Sign analysis (`runInterElimSign`)
- Affine equalities (`runInterElimAffineEqualities`)

The affine domain also exposes queries independent of the solver:
`getConstant(relation, value, side)` returns an optional `APInt` at the value's
integer width. It projects away other variables before checking uniqueness;
congruences that do not determine all bits of the LLVM value, and unreachable
relations, do not produce constants.
`entails(relation, constant, terms)` checks a modular equation whose terms can
refer to `AffineStateSide::Pre` or `AffineStateSide::Post` (the default).
All query coefficients and the constant must use `componentBitWidth()`;
untracked terms or incompatible widths throw `std::invalid_argument`.
Bottom entails every valid equation. `print(relation, stream)` displays the
equations with explicit pre/post operands and their modulus.

Both affine analysis results own their vocabulary. Use result-level
`getConstant`, `entails`, and `print` to query old results after another analysis,
or keep `auto scope = result.scopedVocabulary()` alive while calling domain
operations or `materializeAffineExpressions`. The scope restores the previous
configuration. LLVM modules must still outlive their results. Direct domain
operations use a per-thread configuration.

`expressionPrecondition(relation, constant, terms)` pulls back the expression
`constant + sum(terms)` into an input expression and a feasible-input predicate.
`Exact` means every transition has that expression value; `Unknown` covers
non-functional outputs and non-invertible modular pivots; `Unreachable` means
there are no transitions. This is not a general Boolean weakest-precondition
operator. Even coefficients are never divided as if the coefficient ring were
a field.

The intra/inter clients share LLVM transfer construction in
`Domains/AffineTransfer.cpp`, including casts, selects, predicates, and supported
bitwise congruences. PHIs (including self-loop edges) and actual/formal binding
are parallel assignments. Intra analysis treats ordinary calls conservatively
by forgetting the result; inter analysis retains its own call/return protocol.

Guarded affine relations do **not** generally distribute over affine hull.
The intra client restricts EAN requests to prefix factorization, including when
the caller requests the full Kleene profile, and reports
`ean_laws_restricted` in solve diagnostics. Memo interpretation caches by
expression **and input relation**, preserving ordinary interpretation's merge
points. Every exhausted star reports `NonConvergentStar`, `max_star_hit`, and
the iteration count, for all payload policies. A result with that status is
incomplete; `ReturnLast` does not make it a sound final invariant.

The domain caches Howell reductions and relational compositions per thread.
`setCacheCapacity(0)` disables caching; the default is 128 entries per cache,
with entries cleared on vocabulary changes. `cacheStatistics()` exposes
requests and hits for each operation. Entries are bounded by count rather than
bytes; clients with large vocabularies can lower the capacity. No cross-engine
dependency or WALi runtime dependency is introduced.

These clients reuse the same elimination machinery inside each procedure but
define analysis-specific `callFlow`, `returnFlow`, and `callToRetFlow`
semantics for argument passing, return-value transport, global facts, and
memory effects.

## LLVM pass wrappers

For convenient use under LLVM's legacy pass manager, eight function passes are
provided:

- `-elim-reachable` (reachability)
- `-elim-constprop` (constant propagation)
- `-elim-rd` (reaching definitions)
- `-elim-available` (available expressions)
- `-elim-uninit` (uninitialized variables)
- `-elim-lockset` (may-lockset analysis)
- `-elim-nonnull` (nonnull propagation)
- `-elim-sign` (sign analysis)

Use `-elim-method=state|adt-simple|adt-delayed` to select the solver.
Printing is optional via:

- `-elim-reachable-print`
- `-elim-constprop-print`
- `-elim-rd-print`
- `-elim-available-print`
- `-elim-uninit-print`
- `-elim-lockset-print`
- `-elim-nonnull-print`
- `-elim-sign-print`

Memory modeling can be toggled with:

- `-elim-use-memssa` (default: true) — use MemorySSA to refine memory analyses

When print flags are enabled, pass output includes solver diagnostics:
status, requested/executed method, ADT fallback reason, and star-iteration
counters.

## Solver status and result lookup

- `IntraEliminationSolver::solve()` returns `SolveStatus`:
  `Ok`, `FallbackToState`, `NonConvergentStar`, `InvalidProblem`.
- `IntraEliminationSolver::getDiagnostics()` reports method/fallback/counters.
- `DataFlowResultT` uses explicit read lookup:
  `containsNode(node)` and `tryIN(node)` (nullable pointer); missing nodes have
  no implicit default fact.
- `InterDataFlowResultT<K, ...>` extends the context-sensitive result type with
  `tryIN(inst, ctx)`, `tryOUT(inst, ctx)`, and `contextsForInstruction(inst)`.

## Analysis coverage notes

Constant propagation tracks full LLVM `Constant*` values (integers, floats,
vectors, aggregates), uses LLVM constant-folding and instruction-simplification
when operands are constant, and performs alias-aware memory updates when
`AAResults` are available. Uninitialized-variable tracking normalizes pointer
bases, uses ValueTracking for guaranteed-non-undef checks, and clears aliasing
locations via `AAResults` when available, plus basic mem intrinsics.
