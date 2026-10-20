WPDS (Weighted Pushdown Systems)
================================

Weighted pushdown system solvers used to model interprocedural control flow
with stack-aware summaries.

Overview
--------

The WPDS backend provides algorithms for reasoning about weighted pushdown
systems, which model interprocedural programs with call/return structure.

**Location**: ``third-party/WPDS/``

Features
--------

- Representation of pushdown rules and configuration stacks.
- Extensible weight domains (semirings) for dataflow facts.
- Generalized pushdown reachability and summary computation.

Typical Use Cases
-----------------

- Context-sensitive dataflow analysis with procedure summaries.
- Path-sensitive reasoning over call/return structure.
- Interprocedural program reachability.

Basic Usage (C\+\+)
-------------------

.. code-block:: cpp

   #include <WPDS/WPDS.h>

   // Instantiate wpds::WPDS<T> with an analysis-specific semiring weight.

Integration Notes
-----------------

The WPDS backend is used by higher-level analyses that require interprocedural
reasoning with explicit call stacks (such as APA). See :doc:`index` for a
high-level overview of where WPDS fits in the solver architecture.
