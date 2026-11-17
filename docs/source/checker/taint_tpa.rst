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

External calls need compatible pointer, mod/ref and taint models. In particular,
a memory taint source or pipe needs a corresponding mod/ref effect so the
sparse graph carries its result. Missing mod/ref or taint models, invalid
positions/types and unresolved indirect calls are diagnosed by the CLI.
``IGNORE`` is an explicit model choice. The shipped models provide a starting
point; pin and review the model files used for benchmark comparisons.

The tracker primarily diagnoses call/return context merging. External-model,
alias, path and weak-update imprecision need not yield a loss site. Its output
is a research diagnostic, not a complete explanation of every finding.

Unsupported atomic accesses, exception-handling instructions, ``invoke``,
``callbr``, ``va_arg`` and other instructions outside the validated subset
are rejected. This baseline does not claim coverage of arbitrary C++ programs,
concurrency or whole-program soundness for incomplete external models.

Library use
-----------

Call ``validateModule`` and ``prepareModule`` before building the TPA pointer
program. Keep that program, the LLVM module, the pointer analysis and the sparse
graph alive while using taint results. Do not change the context policy between
analyses. Build the graph with ``DefUseModuleBuilder``, then run
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
