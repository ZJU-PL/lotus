Symbolic Execution
==================

The top-level symbolic execution engine in Lotus backs the
``lotus-check --engine=symex`` checker and other path-sensitive analyses.

**Headers**: ``include/SymbolicExecution/``

**Implementation**: ``lib/SymbolicExecution/``

**Build target**: ``CanarySymbolicExecution``

**Tool frontend**: ``lotus-check --engine=symex`` (see :doc:`../checker/symex`)

Overview
--------

The ``SymbolicExecution`` subsystem is a standalone, path-sensitive symbolic
execution engine. It operates over the guarded value-flow graph (GVFG), tracks
symbolic scalar values and memory facts along feasible paths, and uses SMT-backed
path-condition checks before emitting bug reports.

The subsystem lives at the top level of the source tree (``lib/SymbolicExecution/``
and ``include/SymbolicExecution/``) rather than under ``lib/Analysis/`` because it
owns a full driver, symbolic state model, solver bridge, library modeling layer,
bug-checking engine, and LLVM pass wrapper. It is consumed by the checker
framework through ``SymbolicExecution::SymbolicExecutionWrapper``.

Directory layout
----------------

Headers and implementations mirror one another across seven acyclic layers:

* ``Support/`` – module-scoped ``AnalysisContext``, configurable
  ``AnalysisLimit`` options, and hashing/formatting helpers (``HashUtil``).
* ``Domain/`` – symbolic value and memory abstractions (``BigInteger``,
  ``ProgramVar``, ``PropertyValue``, ``PropertyInteger``, ``PropertySym``,
  ``PropertyAllocator``, ``SymbolicMemory``).
* ``Models/`` – library and taint models (``MemoryAPI``, ``TaintModel``).
* ``Solver/`` – path-condition representation and SMT solving
  (``ConstraintRepr``, ``PathCondSolver``, ``SummarySolverManager``).
* ``Engine/`` – symbolic execution driver, state, summaries, guarded containers,
  C-string and taint state, and extension hooks (``AnalysisDriver``,
  ``AnalysisState``, ``AnalysisSummary``, ``AnalysisExtension``,
  ``GuardedValue``, ``CStringState``, ``TaintState``).
* ``Checks/`` – bug taxonomy, query hierarchy, and checking logic
  (``BugTypes``, ``Query``, ``QueryEngine``, ``MemoryChecks``,
  ``ArithmeticChecks``).
* ``Integration/`` – outer LLVM pass and Lotus report adapter
  (``SymbolicExecutionWrapper``).

Dependencies flow strictly downward: ``Engine`` has no compile-time or link-time
dependency on ``Checks`` (checkers attach via ``AnalysisExtension``), and
``Integration`` sits strictly at the outer boundary.

Core Components
---------------

The engine is composed of the following layers under
``include/SymbolicExecution/``:

Support and models
~~~~~~~~~~~~~~~~~~

* ``Support/AnalysisContext`` – Module-scoped layout, GVFG lookup, and
  call-graph topological ordering.
* ``Support/AnalysisLimit`` – Configurable resource and depth budgets.
* ``Models/MemoryAPI`` – Allocator, deallocator, and known-library recognition.
* ``Models/TaintModel`` – Source, transfer, and sink specifications for
  taint-sensitive bug checks.

Symbolic domain and solver
~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``Domain/ProgramVar`` – Symbolic variables backed by GVFG nodes or auxiliary
  names.
* ``Domain/PropertyValue``, ``Domain/PropertyInteger``, ``Domain/PropertySym``,
  ``Domain/BigInteger`` – Concrete integers and affine symbolic expressions.
* ``Domain/SymbolicMemory`` – Access paths and abstract memory objects.
* ``Solver/ConstraintRepr``, ``Solver/PathCondSolver``,
  ``Solver/SummarySolverManager`` – Path conditions, SMT (Z3) feasibility checks,
  and summary solver pooling.

Execution engine, checks, and integration
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``Engine/AnalysisDriver`` – Whole-module scheduling and summary ownership.
* ``Engine/AnalysisState`` & ``Engine/AnalysisSummary`` – Per-function abstract
  state and interprocedural summary import/export.
* ``Checks/Query`` & ``Checks/QueryEngine`` – Unified bug query construction,
  interprocedural inlining, and MPA batch discharging.
* ``Integration/SymbolicExecutionWrapper`` – LLVM module pass and
  ``BugReportMgr`` emission.

Analysis flow
-------------

A typical symbolic-execution run proceeds as follows:

1. **Context setup** – ``SymbolicExecutionWrapper`` builds an
   ``AnalysisContext`` around the module, data layout, and GVFG builder pass.
2. **Driver scheduling** – ``AnalysisDriver`` computes a bottom-up call-graph
   order and schedules interprocedural execution, reusing ``AnalysisSummary``
   objects across call sites.
3. **Path exploration** – ``AnalysisState`` propagates symbolic values, memory
   objects, C-string lengths, freed-pointer sets, and taint facts over GVFG
   nodes.
4. **Query generation & inlining** – ``QueryEngine`` hooks into
   ``AnalysisState`` via ``StateExtension`` to emit memory and arithmetic safety
   queries and inline callee ``QuerySummary`` payloads.
5. **Feasibility check** – ``PathCondSolver`` discharges path conditions and
   query predicates via SMT (Z3), pruning infeasible paths.
6. **Reporting** – Confirmed ``BugReportRecord`` entries collected by
   ``BugReportCollector`` are emitted by ``SymbolicExecutionWrapper`` into
   ``BugReportMgr`` (JSON/SARIF output).

Usage
-----

The engine is invoked through the unified checker frontend:

.. code-block:: bash

   ./build/bin/lotus-check --engine=symex input.bc
   ./build/bin/lotus-check --engine=symex input.bc --checks=null-deref,use-after-free

For the full list of engine options, see :doc:`../checker/symex`.

Programmatic usage
------------------

The engine is exposed as an LLVM pass via
``SymbolicExecution::SymbolicExecutionWrapper``:

.. code-block:: cpp

   #include "SymbolicExecution/Integration/SymbolicExecutionWrapper.h"

   // Within a pass manager setup:
   auto *seWrapper = new SymbolicExecution::SymbolicExecutionWrapper(
       SymbolicExecution::BUG_TY_BOF | SymbolicExecution::BUG_TY_DBZ);
   pm.add(seWrapper);

Tests
-----

Unit and end-to-end pipeline tests live under ``tests/unit/SymbolicExecution``
and build against ``CanarySymbolicExecution``. They cover the symbolic domain,
library/taint models, call-graph context helpers, and intraprocedural and
interprocedural bug checks.

See Also
--------

* :doc:`../checker/symex` – ``lotus-check --engine=symex`` checker frontend
* :doc:`../ir/gvfg` – Guarded Value-Flow Graph (GVFG) IR
* :doc:`../solvers/smt` – SMT (Z3) solver backend
* :doc:`../user_guide/architecture` – Architecture overview
