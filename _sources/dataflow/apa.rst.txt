Algebraic Program Analysis (APA)
================================

``APA`` is Lotus's elimination-based dataflow analysis (or, "algebraic program analysis") framework.

**Headers**: ``include/Dataflow/APA/``

**Implementation**: ``lib/Dataflow/APA/``

Overview
--------

APA expresses classical dataflow problems as algebraic program analysis and solves
them with elimination-style algorithms. The framework currently provides LLVM
clients for standard intraprocedural analyses and reusable solver backends.

Main components
---------------

- ``Core/`` defines generic problem, result, and option abstractions.
- ``Solver/`` contains solver implementations such as state elimination,
  ADT-simple, and ADT-delayed solvers.
- ``EAN/`` implements the equality-saturation optimizer for path-expression
  DAGs: import, saturation, extraction, and export.
- ``Baseline/TranslAPA/`` provides the TranslAPA baseline, a closed-form
  Gen/Kill semiring fold over the elimination front-end's path expressions.
- ``Domains/`` defines the abstract fact types and their lattice operations.
- ``LLVM/`` bridges LLVM CFGs, dominance information, and call resolution to
  the generic APA problem interfaces.
- ``Analyses/Intra/`` provides ready-made intraprocedural analyses:

  - available expressions
  - affine equalities
  - constant propagation
  - lockset analysis
  - non-null
  - reachability
  - reaching definitions
  - sign analysis
  - uninitialized variables

- ``Analyses/Inter/`` provides the same nine analyses as ``Analyses/Intra/``.
  Eight use the common call-string worklist and forward-summary solvers;
  interprocedural affine equalities uses its module-scoped driver.

- ``Passes/EliminationPasses.h`` exposes LLVM-pass integration.

Order-aware elimination
-----------------------

``EliminationOptions`` and ``PathSummaryEquationOptions`` expose ``Ordering``
and ``Order`` settings. The policies ``Structural``, ``ExpressionAware``,
``StarRisk``, and ``Hybrid`` use live-graph sparse elimination, cached operand
DAG sizes, and a dirty-candidate/versioned-heap selector. Removed equations are
back-substituted so summaries remain available at every program point. These
policies are supported in both equation directions, the forward-summary solver,
and the modular summary builder. Module-scoped interprocedural affine equalities
forwards ``InterAffineEqualitiesOptions::ordering`` and ``order`` to its
per-procedure solver.

.. code-block:: cpp

  elimination::PathSummaryEquationOptions Options;
  Options.Ordering = elimination::OrderingPolicy::Hybrid;
  Options.Order.RecordTrace = true;
  Options.Order.MeasureLiveNodes = true;
  auto Result = elimination::runInterSummaryElimReachability(Entry, nullptr, Options);

Use ``lotus-dfa-apa --ordering=hybrid --measure-peak --order-trace --stdout``
to emit candidate scores, pivot traces, live-DAG statistics, allocations, and
ranking costs. ``--inter-engine=context|expanded|modular`` selects the model.
Context mode caches construction per procedure/call-string pair and reinterprets
after external fact updates. ``--order-full-rescore`` checks incremental selection.
Policies never omit signals or substitute Structural; no signal-disabling or
metadata-fallback flags are provided.
Additional baselines are ``rpo``, seeded ``random``, ``min-degree``, and complete
local permutations via ``explicit --order-explicit=0,1,...``.

Historical defaults are preserved. Use ``--order-sparse`` on **all** compared
configurations to isolate ordering from the legacy full-matrix engine. Online
ordering with an ADT engine is rejected rather than silently switching engines. The
sparse equation baseline uses ascending local SCC indices; sparse intra Default
keeps its historical permutation. Live-DAG counts include saved equations and query
summaries, but exclude factory-only roots; active-graph counts are separate. Neither
measures physical RSS. Nested semantic-star intervals are counted only once.

Normalization caps and operand-size limits are configurable. Their defaults
are untuned engineering values, not empirical results. Fact equivalence across
orders requires a language-invariant interpretation; a lattice interface alone
does not guarantee it. See ``lib/Dataflow/APA/README.md`` for policy formulas,
semantic-cache snapshots, measurement scopes, and the responsibility-based header
layout. Core strategies have separate ``Ordering/Policies/`` headers; signal
collection and versioned selection are independent modules. No ``Detail`` directory
or backward-compatibility headers are retained.

Interprocedural Forward Summary Solver
--------------------------------------

The ``ForwardInterSummarySolver`` is a context-sensitive interprocedural
solver that encodes the entire program as a single global equation graph.
It solves the graph by computing SCCs and evaluating closed-form regular
path expressions, rather than iterating per-procedure worklists to
convergence.

Conceptual model
^^^^^^^^^^^^^^^^

The solver builds a ``PathSummaryEquationGraph`` whose nodes are
``(Instruction, CallStringContext)`` pairs. Edges are labeled with
``InterSummaryTransferAtom`` values that reify interprocedural transfer
effects. There are four atom kinds:

- ``RawNormal`` — ordinary intraprocedural edge transfer.
- ``CallEntry`` — transfer from a call site into a callee entry point
  (``callFlow`` in the problem interface).
- ``ReturnExit`` — transfer from a callee exit back to a return site
  (``returnFlow``).
- ``CallToRet`` — bypass transfer that skips the callee and flows
  directly from call site to return site (``callToRetFlow``).

The resulting system of left-linear equations has the form:

.. code-block:: text

  X_v = base_v  U  (X_u . W_u,v)

where ``base_v`` captures contributions from inter-SCC predecessors and
seed facts, and ``W_u,v`` are path expressions composed from the atom
types above.

Contrast with the worklist solver
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

APA provides two interprocedural solver engines. The worklist-based
``InterEliminationSolver`` wraps each function as an intraprocedural
problem and propagates across call sites through a global worklist.
The summary-based ``ForwardInterSummarySolver`` takes a different
approach:

+-----------------------------------+------------------------------------+
| Worklist solver                   | Summary solver                     |
| (``InterEliminationSolver``)      | (``ForwardInterSummarySolver``)    |
+===================================+====================================+
| Solves one procedure at a time    | Builds one global equation graph   |
| via ``ProcedureProblemAdapter``   | covering all (inst, context) nodes |
+-----------------------------------+------------------------------------+
| Converges by iterating the        | Closes recursive SCCs with a       |
| worklist until facts stabilize    | ``Star`` expression operator       |
+-----------------------------------+------------------------------------+
| Sequential per-procedure solving  | Solves SCCs in dependency order    |
+-----------------------------------+------------------------------------+
| No SCC decomposition              | Tarjan SCC decomposition followed  |
|                                   | by topological layer ordering      |
+-----------------------------------+------------------------------------+
| Cyclic call graphs handled by     | Cyclic SCCs use Floyd-Warshall-    |
| re-enqueuing changed nodes        | style closure over path expressions|
+-----------------------------------+------------------------------------+
| Context-cached tool default       | ``--inter-engine=expanded``        |
| Build once, reinterpret facts     | selects the global equation model  |
| after call/return fact updates    | (also available as a library API)  |
+-----------------------------------+------------------------------------+

Supported analyses
^^^^^^^^^^^^^^^^^^

Eight interprocedural analysis entry points provide both worklist and
summary solver variants:

``runInterSummaryElimAvailableExpressions``
  ``(Function *Entry, const InterCFG *, PathSummaryEquationOptions)``

``runInterSummaryElimReachability``
  ``(Function *Entry, const InterCFG *, PathSummaryEquationOptions)``

``runInterSummaryElimConstantPropagation``
  ``(Function *Entry, AAResults *, AssumptionCache *, DominatorTree *,``
  ``TargetLibraryInfo *, const InterCFG *, PathSummaryEquationOptions)``

``runInterSummaryElimReachingDefinitions``
  ``(Function *Entry, AAResults *, MemorySSA *, const InterCFG *,``
  ``PathSummaryEquationOptions)``

``runInterSummaryElimUninitializedVariables``
  ``(Function *Entry, AAResults *, AssumptionCache *, DominatorTree *,``
  ``const InterCFG *, PathSummaryEquationOptions)``

``runInterSummaryElimLockset``
  ``(Function *Entry, const InterCFG *, PathSummaryEquationOptions)``

``runInterSummaryElimNonNull``
  ``(Function *Entry, AssumptionCache *, DominatorTree *, const InterCFG *,``
  ``PathSummaryEquationOptions)``

``runInterSummaryElimSign``
  ``(Function *Entry, const InterCFG *, PathSummaryEquationOptions)``

Each pair-solver variant shares the same analysis domain and fact types
with its worklist counterpart, making it straightforward to switch
between engines for differential validation.

Key classes
^^^^^^^^^^^

``ForwardInterSummarySolver<AnalysisTypesT, K>``
  Main solver template. Owns a ``PathSummaryEquationGraph``, discovers
  equation nodes by traversing the interprocedural CFG from seed facts,
  delegates to ``PathSummaryEquationSolver`` for solving, and evaluates
  the resulting summaries into ``InterDataFlowResultT`` facts. Lives in
  ``include/Dataflow/APA/Solver/Inter/ExpandedSolver.h``.

``PathSummaryEquationSolver<KeyT, TransferT>``
  Generic engine that computes Tarjan SCCs on the equation graph, orders
  them by dependency, and solves them sequentially. Cyclic SCCs are solved
  with a Floyd-Warshall-style closure over path expressions using ``Star``,
  ``Concat``, and ``Union`` operators. Lives in
  ``include/Dataflow/APA/Solver/Equations/Solver.h``.

``InterSummaryTransferAtom<AnalysisTypesT>``
  First-class atom that tags an edge as one of the four transfer kinds
  (RawNormal, CallEntry, ReturnExit, CallToRet). Constructed via static
  factory methods (``rawNormal``, ``callEntry``, ``returnExit``,
  ``callToRet``).

``InterSummaryTransferEvaluator<AnalysisTypesT, K>``
  Evaluates a path expression built from ``InterSummaryTransferAtom``
  labels against an input fact. Dispatches each atom kind to the
  corresponding problem hook (``applyTransfer``, ``callFlow``,
  ``returnFlow``, ``callToRetFlow``) and handles ``Star`` by iterating
  until fixpoint.

Usage example
^^^^^^^^^^^^^

.. code-block:: cpp

  #include "Dataflow/APA/APA.h"

  using namespace elimination;

  // Build the interprocedural CFG for the module.
  auto ICF = dataflow::controlflow::InterCFG::build(*Module);

  // Run the summary-based reachability analysis.
  auto Result = runInterSummaryElimReachability(Main, ICF.get());

  // Inspect the result at a program point.
  for (auto &Inst : instructions(*Main)) {
    if (auto *In = Result.tryIN(&Inst, {})) {
      // Inst is reachable from the entry point.
    }
  }

  // Diagnostics include equation graph statistics.
  auto &Diag = Result.summarySolveDiagnostics();
  assert(Diag.equation_node_count > 0);
  assert(Diag.scc_count > 0);

The unit tests in ``tests/unit/Dataflow/APA/`` validate parity between the
worklist and summary solvers across the supported analysis types, including
recursive call-graph patterns.

Library-only availability
^^^^^^^^^^^^^^^^^^^^^^^^^^

The tool defaults to the context-cached ``InterEliminationSolver``. Each
procedure/call-string pair's expressions are built once, then reinterpreted
with a fresh input-sensitive memo when external facts change. Select the global
equation model using ``--inter-engine=expanded``, or call
``runInterSummaryElim*`` directly. ``--inter-engine=modular`` selects the
functional/context-insensitive alternative for reachability.

EAN equality saturation
-----------------------

``EAN`` (namespace ``elimination::ean``) is the compiler-style optimizer pass
between path-expression construction and interpretation. It imports a batch of
path-expression roots ``R`` into a canonical e-graph, runs guarded and budgeted
saturation, extracts the cheapest equivalent shared DAG under a cost model, and
exports it back into a ``PathExprFactory``. The entry point lives in
``include/Dataflow/APA/EAN/EAN.h``:

.. code-block:: cpp

  template <typename TransferT>
  std::vector<typename PathExprFactory<TransferT>::Ref>
  ean(const std::vector<typename PathExprFactory<TransferT>::Ref> &R,
      const LawProfile &L, const CostModel &C, const Budget &B,
      PathExprFactory<TransferT> &F, SaturationStats *stats = nullptr,
      ExtractOptions opts = {});

The pipeline has four stages:

- ``Import`` (``Import.h``) canonicalizes the batch into an e-graph. Variadic
  joins are flattened, deduplicated, and sorted; variadic sequences are
  flattened but never reordered. Every input root maps to an e-class id, so the
  original expression is always recoverable (root preservation).
- ``Saturate`` (``Saturate.h``) runs the phased rewrite schedule Cleanup,
  Factor, Star, Explore, each to local saturation, under the ``Budget`` bounds
  on e-nodes, applied matches, rounds, wall-clock time, and consecutive rounds
  without cost improvement. The driver is anytime: any stopping point yields a
  valid extractable result.
- ``Extract`` (``BatchExtract.h``) picks one e-node per e-class to minimize the
  reuse-aware shared-DAG objective (Eq. 5), using a cycle-safe relaxation
  followed by a reuse-refinement loop.
- ``Export`` (``Export.h``) materializes the chosen representatives back into
  the factory, recovering cross-root sharing through a memo keyed by canonical
  e-class id. Atoms are re-exported verbatim and stay opaque to EAN.

Rewrites are gated by a client-declared ``LawProfile`` (``LawProfile.h``):
left/right distributivity, annihilation, Kleene-star laws, unfoldings, and
sliding. Presets include ``kleeneAlgebra()``, ``flowAlgebra()``, and the
universally-safe ``safeMinimal()`` (left distributivity only). The ``CostModel``
(``CostModel.h``) supplies per-operator weights plus the Eq. 5 shared-DAG
objective weights; presets are ``uniform()``, ``profiled()``, and ``dag()``.

EAN is fail-safe. If anything throws, the result shape is wrong, or the
optional monotone guard detects a larger output, ``ean()`` returns the original
root batch unchanged. An invocation gate skips saturation entirely below a
minimum unique-node threshold. The modular interprocedural solver uses EAN as
an optional per-procedure post-pass (see below).

TranslAPA baseline
------------------

``TranslAPA`` (namespace ``elimination::translapa``) is a baseline that keeps
the elimination front-end verbatim and swaps only the interpreter. Instead of
``SolverContext::eval``, which re-applies generic transfers and iterates
``Star`` to a lattice fixpoint, it folds each node's path expression with the
closed-form Gen/Kill semiring (paper §4) in a single memoized bottom-up pass.

The reusable core is ``foldFillGenKill()`` (``Driver.h``). Given a solved
result and a translator that maps transfer atoms to ``(Gen, Kill)`` pairs, it
overwrites every node's IN fact with the semiring interpretation and returns
the fold wall-clock time in microseconds. The timed variant
``foldFillGenKillTimed()`` splits mechanical extraction from the per-query fold
so the two costs are measured apart.

The semiring (``GenKillSemiring.h``) represents a program property as a pair of
fixed-width ``llvm::BitVector`` subsets of the finite fact universe, denoting
the transfer function ``f(x) = Gen | (x & ~Kill)``. Composition is closed form:
``seq`` unions the kills and subtracts the second kill from the first gen,
``join`` unions gens and intersects kills, and ``star`` is O(1) with no
iteration. This is the key contrast with the generic tree-walking interpreter.

The mechanical translation (``AtomTranslator.h``) recovers each atom's
``(Gen, Kill)`` by probing the client's own ``applyTransfer``: ``Gen = f({})``
and fact ``d`` is killed iff ``d`` is not in ``f({d})``, so no per-client hand
translation is required. A separable fast path uses two probes per atom instead
of the general singleton probing.

The fold interpreter (``FoldInterpreter.h``) memoizes on the ``Expr`` pointer,
so cost is O(#unique DAG nodes) rather than a recursion over the expanded tree.
Same path expression, different semantic function: that is the apples-to-apples
comparison this baseline exists for.

Modular interprocedural summary solver
--------------------------------------

The ``ModularInterSummaryBuilder`` (E6, milestone 3) is the modular counterpart
of the monolithic ``ForwardInterSummarySolver`` described above. Instead of
encoding the whole program as one global equation graph, it builds ONE
entry-to-exit path-expression summary per reachable non-opaque procedure over
that procedure's own CFG. Call sites become symbolic
``SummaryCall(callsite, callee, retsite)`` atoms rather than inlined callee
bodies, which keeps construction linear in program size: each summary is solved
on a bounded per-procedure graph, never on the whole-program x context product
that makes the monolithic solver blow up.

Per-procedure summaries are solved with ``PathSummaryEquationSolver`` over
``InterSummaryTransferAtom`` labels, which already handles intraprocedural loops
via ``Star``. Each ``ProcSummary`` carries the entry-to-exit expression (a union
over the procedure's exit points) plus an entry-to-node expression for every
instruction. The builder reports aggregate ``DagStats`` before and after an
optional per-procedure EAN or Greedy post-pass, mirroring the monolithic
solver's Table VI/VII diagnostics.

Recursion is deliberately deferred. A ``SummaryCall`` to a same-team procedure
is an opaque placeholder during construction; closing recursive teams is an
interpretation-time fixpoint (milestone 4) guided by the reverse-topological
order and ``recursive`` flags in the returned ``CallGraphSCCResult``.

The ``ModularInterSummaryDriver`` (milestone 4) interprets the summaries into
IN/OUT facts with a context-insensitive, functional fixpoint over procedure
entry facts: entry procedures start at the initial fact, callee entry facts
accumulate via ``callFlow`` at their call sites, and the outer loop iterates
until neither the entry facts nor the interpreter's recursion memo changes.
Results use the empty context key, one IN per instruction. The interpreter
evaluates a ``SummaryCall`` as ``In -> callFlow -> callee entry-to-exit ->
returnFlow``, closing recursion with a memoized fixpoint that evaluates each
``(callee, exit, input-fact)`` at most once per pass.

The tool exposes this model using ``--inter-engine=modular`` and the expanded
equation model using ``--inter-engine=expanded``. Context-cached mode is the default.

Related engines
^^^^^^^^^^^^^^^

- See :doc:`wpds` for the WALi pushdown backends and :doc:`npa` for the
  sparse Newton solver.
- See :doc:`mono` and :doc:`ifds_ide` for the other dataflow engines.
- See :doc:`../tools/dataflow/index` for the testing front-ends.

Typical use cases
-----------------

- Compare elimination-based solving with Mono or IFDS engines.
- Prototype intraprocedural dataflow problems over LLVM IR.
- Drive differential-testing workflows for solver validation.

Analysis Tooling & Evaluation
-----------------------------

The command-line driver (``lotus-dfa-apa``) logic is cleanly separated into reusable components under ``include/Dataflow/APA/Tooling/`` and ``lib/Dataflow/APA/Tooling/``. This isolation prevents the core APA analysis engines from being polluted with experiment-specific artifacts (like repetitive cycle timing, specific OS memory profiling tools, or hardcoded benchmark variants), offering a clean entry point for client applications.

See also
--------

- See :doc:`mono`, :doc:`ifds_ide`, and :doc:`npa` for related engines.
- See :doc:`../tools/dataflow/index` for the testing front-ends.
