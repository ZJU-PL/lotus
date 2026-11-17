TPA Sparse Taint Research Baseline
==================================

``lotus-check --engine=taint-tpa`` runs an explicit-flow taint baseline ported from
Jia Chen's TPA taint engine and adapted to LLVM 14. It uses TPA's native memory
objects and context-specific points-to queries. It supports experiments on
sparse propagation, context sensitivity and precision-loss tracking.

**Library**: ``TPATaint``

**Implementation**: ``include/Checker/TPA/`` and ``lib/Checker/TPA/``

**Namespace**: ``tpa::taint``

Build and run
-------------

.. code-block:: bash

   cmake --build build --target lotus-check tpa_taint_tests -j4
   ctest --test-dir build -R '^tpa_taint' --output-on-failure

   build/bin/lotus-check --engine=taint-tpa input.bc --taint-tpa.k-limit=1 --analysis-stats
   build/bin/lotus-check --engine=taint-tpa input.bc --taint-tpa.k-limit=0 --taint-tpa.track-precision-loss

A focused build can select ``-DLOTUS_TEST_SUBSYSTEMS=checker/tpa`` and
``-DLOTUS_TOOL_FAMILIES=checker``. The regression tests exercise both the
native API and the command-line tool.

The tool follows the shared checker exit policy: 0 on success, 1 when
``--fail-on-findings`` is set and retained findings exist, and 2 for diagnosed
input/model/report errors. Absence of a finding is relative to the supplied
models and the supported IR subset. Reports support the common JSON, SARIF,
suppression and minimum-score options. Native results carry the ``taint-tpa``
engine metadata, argument index and calling context.

Options
-------

* ``--taint-tpa.unmodeled=ignore|error`` selects the missing external model
  and unresolved-call policy. The default is ``ignore``, an optimistic research
  setting. ``error`` requires all models and call targets.

* ``--taint-tpa.k-limit=N`` uses the same k-limited call-string policy for pointer and
  taint propagation. The default is 0, merging calling contexts.
* ``--taint-tpa.ptr-config=FILE``, ``--taint-tpa.modref-config=FILE`` and ``--taint-tpa.taint-config=FILE``
  select external models. Defaults are ``ptr.spec``, ``modref.spec`` and
  ``taint.spec`` under ``LOTUS_CONFIG_DIR`` when set; otherwise the installed
  configuration directory, falling back to the source configuration directory.
* ``--analysis-stats`` reports static SSA/memory edge counts, forward evaluations,
  contexts, value facts, memoized memory states, sink instances, loss sites,
  raw sink-violation counts and wall-clock phase timings. The taint timing includes optional tracking
  and graph output. Pointer timing includes normalization and program building.
* ``--taint-tpa.def-use-dot-dir=DIR`` writes the sparse graphs. Dotted edges carry SSA
  dependencies; labeled edges carry individual TPA memory objects. Numeric
  filenames avoid interpreting function names as paths; each graph contains
  its function name.
* ``--taint-tpa.track-precision-loss`` traces ambiguous sink findings back through the
  upstream precision tracker. This produces candidate loss sites, without
  automatically refining contexts or proving a reported path feasible.
* ``--taint-tpa.no-prepass`` skips optional TPA normalization. Return unification still
  runs because TPA's pointer CFG requires one exit node per function.

Pipeline and semantics
----------------------

The pipeline normalizes LLVM IR, unifies function exits, runs TPA pointer
analysis, computes interprocedural mod/ref summaries and reaching definitions,
constructs SSA and memory def-use edges, then solves taint on that sparse graph.
Graph construction merges points-to information across contexts; taint loads,
stores and indirect calls query the pointer solution in the current context.
Only functions reachable from ``main`` through TPA's call targets and basic
blocks reachable from their entries are included in the sparse graph.

SSA taint is stored by ``(context, LLVM value)``. Memory taint uses TPA memory
objects with incoming stores memoized by ``(context, def-use instruction)``.
A store to one concrete nonsummary object overwrites its taint; multiple or
summary targets receive weak updates. Field separation and array summarization
follow TPA's memory model.

The original four-element lattice is retained: ``Unknown`` is bottom,
``Untainted`` and ``Tainted`` are incomparable, and ``Either`` is their join.
Both ``Tainted`` and ``Either`` violate an untainted sink policy. Operand
propagation also uses this join, so combining a tainted operand with a clean
constant can yield ``Either``. It does not establish that the instruction is
safe or that any particular execution reaches a sink. Uninitialized or
uncomputed memory can remain ``Unknown``; it is not a separate vulnerability
class in this baseline.

``argc`` is initially tainted; ``argv``/``envp`` pointer values are clean and
their modeled contents are tainted. Defined global contents start clean.
Control conditions do not introduce implicit taint. Branch alternatives are
joined without path-feasibility checks.

Models and experimental controls
--------------------------------

The baseline retains the upstream annotation representation under
``Checker/TPA/Annotation``. It reads the existing text specification syntax;
it uses its own annotation representation and exports findings through the
shared checker reporting infrastructure.
Function names are matched exactly against LLVM symbol names.

.. code-block:: text

   SOURCE source Ret V T
   SINK sink Arg0 V
   SOURCE fill Arg0 D T
   PIPE copy Arg0 R Arg1 R
   IGNORE trusted_noop

``V`` denotes a value, ``D`` directly pointed-to memory and ``R`` the modeled
objects in the same allocation block. ``R`` is not an arbitrary transitive
heap traversal. ``Ret``, ``ArgN`` and ``AfterArgN`` specify return values,
individual arguments and argument ranges. Source values are ``T`` (tainted),
``U`` (untainted) or ``E`` (either). Pipes put the destination before the source.
Sinks support ``V`` and ``D``. Reachable-memory pipes copy offsets for individual
``R``-to-``R`` arguments; other pipes reduce source taint before updating the
destination. Memory effects at external calls are weak updates. Legacy scalar-return
effects on void calls are ignored, matching the pointer client.

For mixed variadic inputs, paired value and memory rules are used:

.. code-block:: text

   PIPE snprintf Arg0 D AfterArg2 V
   PIPE snprintf Arg0 D AfterArg2 D

The value rule carries scalar argument taint (and pointer-value taint). The
memory-input range selects only pointer arguments, so numeric arguments are
never passed to points-to queries. Mod/ref argument ranges likewise select
pointers; the shipped formatted-output summaries include matching reads, such
as ``snprintf REF AfterArg2 D``, to construct sparse memory dependencies for
string arguments. Fixed memory positions, taint source/sink positions and
memory pipe destinations still require pointer operands. These are type-directed
summaries; they do not parse format strings or unpack ``va_list`` contents.

Indirect call candidates are filtered by calling convention, argument count,
return type and fixed argument types in both the pointer solver and call-target
queries. Compatible typed-pointer casts are retained when address spaces match;
variadic targets accept additional arguments after their fixed prefix. This is
a candidate filter, not a proof of full ABI compatibility. Explicit named
callees, including casts of known function symbols, retain their identity.
``filtered_indirect_targets`` counts eliminated merged-context candidates per
static call site. If none remain, the unresolved-call policy applies.

Available external models are applied independently. When a taint model is
missing in ``ignore`` mode, the call generates no taint and performs no taint
kills; its new SSA return value is assumed untainted so dependent internal
calls can still be analyzed. Existing argument and memory taint survives.
Missing mod/ref models add no memory effects. This policy can miss real flows
through unmodeled functions. Explicit ``IGNORE`` taint entries use the same
no-effect/untainted-return behavior and are accepted in strict mode.

LLVM debug declaration, value and label intrinsics are known no-ops in both
modes. They are excluded from model validation, call reachability and the
sparse def-use graph, and do not count as ignored externals. Debug locations
remain available for reports.

``--analysis-stats`` records the policy, unique ignored symbols, distinct static
call sites, and per-symbol missing taint/mod-ref counts. An indirect call with
multiple missing targets counts once in the total call-site count and once
under each affected symbol. Worklist revisits do not inflate counts. A warning
summarizes missing-model coverage unless logging is disabled; finding metadata
also records the policy and aggregate counts.

Unresolved calls, including inline assembly, follow the same policy. In
``ignore`` mode they preserve incoming taint and memory facts and assume an
untainted new SSA return value; known callees continue to use their bodies or
models. Statistics distinguish ignored unresolved call sites and inline-assembly
sites, with per-caller counts. The counts include context-specific resolution
failures encountered by the solver and do not count worklist revisits.
``TaintAnalysis::getIgnoredUnresolvedCalls`` exposes the executed call sites.
Strict mode diagnoses a missing target, including the caller and call IR.

The pointer analysis continues using its existing external-pointer models and
fallback behavior; this option controls the taint and mod/ref layers. A memory
taint source or pipe still needs a corresponding mod/ref effect so the sparse
graph carries its result. Invalid available models remain errors. Pin and
review the model files and policy used for benchmark comparisons.

The tracker primarily diagnoses call/return context merging. External-model,
alias, path and weak-update imprecision need not yield a loss site. Its output
is a research diagnostic, not a complete explanation of every finding.

Supported LLVM data operations include arithmetic and casts, ``fneg``, scalar
and pointer ``freeze``, aggregates/vectors, loads/stores and scalar atomics.
Indirect branches carry existing explicit data flows without introducing
implicit control taint. ``freeze`` preserves operand taint; poison/undef
nondeterminism is not modeled separately.

Atomic loads/stores use the corresponding ordinary taint memory effects.
``atomicrmw`` returns the old contents and writes a value derived from its
operation; exchange can strongly overwrite a concrete singleton object.
``cmpxchg`` joins retained and replacement memory because success is not proved,
and conservatively combines old-value and comparison taint in its aggregate
result. ``fence`` changes no taint facts. These are sequential data effects:
thread scheduling, interference and memory-order guarantees are not analyzed.
Pointer-valued RMW/compare-exchange remain unsupported because their pointer
store effects also need implementation in the TPA backend.

Exception-handling instructions, ``invoke``, ``callbr``, ``va_arg`` and other
instructions outside the validated subset are rejected. This baseline does not
claim coverage of arbitrary C++ programs, concurrency or whole-program soundness
for incomplete external models.

Library use
-----------

Call ``validateModule`` and ``prepareModule`` before building the TPA pointer
program. Keep that program, the LLVM module, the pointer analysis and the sparse
graph alive while using taint results. Do not change the context policy between
analyses. Pass the same ``UnmodeledPolicy`` to ``DefUseModuleBuilder`` and
``TaintAnalysis`` (both default to ``Ignore``), then run
``TaintAnalysis::runOnDefUseModule(graph, trackPrecisionLoss)``.

The analysis exposes its environment, incoming memory stores, violations,
precision-loss sites and counters through read-only getters. Re-running clears
previous taint results. ``TrackingTaintAnalysis`` provides a compatibility
facade over the same forward solver.

Migration changes
-----------------

The port replaces obsolete LLVM call-site wrappers with ``CallBase``, preserves
the upstream MIT license, and fixes memory-copy direction and reading of source
objects, multiple taint exits, late caller return replay, conditional write
pass-through and global field initialization. Exit unification satisfies the
existing pointer frontend's single-return requirement. Configuration parsing
supports final comments and bounded indices; mod/ref parsing accepts both
``IGNORE name`` and the shipped ``name IGNORE`` spelling.

A small context-sensitivity example is in
``tests/unit/Checker/TPA/Inputs/context.ll``. Use the three ``.spec`` files in
that directory with the corresponding CLI options. It produces two sink
violations at k=0 and one at k=1.
