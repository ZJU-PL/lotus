SMT (Satisfiability Modulo Theories)
===================================

SMT solving infrastructure with Z3 integration and support for common theories.

Overview
--------

The SMT backend provides a unified interface for building and solving SMT
formulas over a variety of theories.

**Location**: ``lib/Solvers/SMT/``

**Main capabilities**:

- Z3-based SMT solver backend.
- Theory support for arithmetic, arrays, bit-vectors, and uninterpreted
  functions.
- Utilities for incremental solving and formula construction.

Typical Use Cases
-----------------

- Encoding verification conditions and safety properties.
- Solving constrained Horn clauses (CHCs) produced by analyses and front-ends.
- Implementing symbolic execution and path feasibility checks.

Basic Usage (C\+\+)
-------------------

.. code-block:: cpp

   #include <Solvers/SMT/SMTContext.h>

   SMTContext Ctx;
   auto Solver = Ctx.createSolver();
   auto Phi = /* build formula */;
   Solver->add(Phi);

Features
--------

- **Z3 integration** – Tight integration with Z3 4.11 as the primary SMT
  backend.
- **Theory solvers** – Support for integer and real arithmetic, arrays,
  bit-vectors, and uninterpreted functions.
- **Formula construction** – Helper APIs for building and manipulating SMT
  expressions.
- **Incremental solving** – Push/pop interfaces for iterative solving.

Integration Notes
-----------------

The SMT backend is used throughout Lotus by components that require reasoning
over rich theories, including CHC-based verifiers and symbolic abstractions.
See :doc:`solvers` for a high-level overview of where the SMT solver fits in
the rest of the system.


