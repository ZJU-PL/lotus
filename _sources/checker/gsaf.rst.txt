GSAF
====

GSAF is a path-sensitive bug-finding engine over Lotus's guarded value-flow
IR (GVFG).

Run it through the unified checker frontend::

   lotus-check --engine=gsaf --checks=taint,use-after-free input.bc
   lotus-check --engine=gsaf --checks=all --report-json=report.json input.bc

The engine implementation is in ``lib/Checker/GSAF``. Library contracts reuse
``Annotation/APISpec`` and ``Annotation/Taint``. Build the ``GSAF`` library or
``lotus-check`` executable. Run existing component tests with::

   build/bin/tests/checker_tests --gtest_filter='GSAF*'

Organization
------------

Public headers mirror the implementation directories:

* ``API``: vulnerability contracts, traces, registration and annotation model queries.
* ``Engine``: four header/source pairs for the module checker, taint traversal,
  solver and summaries.
* ``Checkers``: the two built-in checks (use after free and taint).
* ``Report``: diagnostic rendering and trace scoring.
* ``Support``: GSAF-specific options, queries, masks and object ordering.

Reusable analyses and reporting primitives live in their respective Lotus
subsystems. Framework diagnostic events and composition extend
``DiagnosticEvent`` and ``CheckerDiagnostic``; all report steps use ``BugReport``.
LLVM/GVFG narrative rendering belongs to GSAF. See ``lib/Checker/GSAF/README.md`` for the dependency boundaries.
The per-function engine is ``Engine/FunctionAnalyzer``. Checker trace containers,
reporting metadata, and the LLVM value-pair representation live in
``Checker/Framework/ValueTrace.h`` and ``Checker/Framework/LLVMValueTrace.h``.
Graph-specific traces and conversion live in ``IR/GVFG``.
