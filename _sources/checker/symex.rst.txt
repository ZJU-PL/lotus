Symbolic Execution Checker
==========================

The symbolic-execution checker is exposed through the ``lotus-check --engine=symex``
engine mode and is backed by the top-level ``SymbolicExecution`` subsystem.

**Engine Location**: ``lib/SymbolicExecution/``

**Headers**: ``include/SymbolicExecution/``

**Tool Frontend**: ``tools/checker/lotus-check-symex.cpp``

Overview
--------

The engine performs path-sensitive symbolic execution over the guarded
value-flow graph. It tracks symbolic scalar values, access paths, guarded
memory facts, taint-derived facts, and path conditions, then uses SMT-backed
feasibility checks before emitting bug reports.

The main build target is ``CanarySymbolicExecution``. The target remains
separate from ``lib/Analysis`` because the subsystem owns a full driver,
symbolic state model, solver bridge, memory modeling layer, taint model, and
LLVM pass wrapper.

Core Components
---------------

* ``Support/AnalysisContext`` & ``Support/AnalysisLimit`` – Module-scoped
  layout/GVFG/call-graph context and configurable resource budgets.
* ``Domain/ProgramVar``, ``Domain/Property*``, and ``Domain/SymbolicMemory`` –
  Symbolic variables, affine scalar expressions, and abstract memory objects.
* ``Models/MemoryAPI`` & ``Models/TaintModel`` – Allocator/deallocator models
  and taint source, transfer, and sink specifications.
* ``Solver/ConstraintRepr``, ``Solver/PathCondSolver``, and
  ``Solver/SummarySolverManager`` – Path conditions and SMT feasibility checks.
* ``Engine/AnalysisDriver``, ``Engine/AnalysisState``, and
  ``Engine/AnalysisSummary`` – Whole-module scheduling, symbolic transfer,
  taint propagation, and interprocedural summaries.
* ``Checks/Query`` & ``Checks/QueryEngine`` – Bug query construction, summary
  inlining, and MPA batch discharging via ``AnalysisExtension`` hooks.
* ``Integration/SymbolicExecutionWrapper`` – Integrates the engine with the
  LLVM pass pipeline and checker report infrastructure.

Supported checks
----------------

``symex`` can select checks with ``--checks``.  The available names
are ``buffer-overflow``, ``div-by-zero``, ``int-overflow``, ``int-underflow``, ``null-deref``,
``signed-int-overflow``, ``signed-int-underflow``, ``shift-overflow``,
``array-oob``, ``uninitialized-read``, ``use-after-free``, ``double-free``,
``negative-array-index``, and ``int-truncation``.  Multiple names are supplied
as a comma-separated list.

Usage
-----

.. code-block:: bash

   ./build/bin/lotus-check --engine=symex input.bc
   ./build/bin/lotus-check --engine=symex input.bc --checks=null-deref,use-after-free

Scope and alternatives
----------------------

SymEx overlaps with several specialized engines, but provides a different
trade-off: it uses symbolic path conditions and SMT feasibility checks at a
potentially higher cost.  Prefer ``kint`` for routine numerical-bug analysis,
``pulse`` for bounded witness-oriented memory-safety diagnosis, and ``saber``
for leak or double-free value-flow checks.  See
:ref:`Choosing a Checker <choosing-a-checker>` for the complete guide.

Tests
-----

Focused unit tests live under ``tests/unit/SymbolicExecution`` and build
against ``CanarySymbolicExecution``.

See Also
--------

* :doc:`../symbolic_execution/index` – Symbolic execution engine documentation
* :doc:`index` – Checker framework overview
* :doc:`../tools/checker/index` – ``lotus-check`` command-line frontend
