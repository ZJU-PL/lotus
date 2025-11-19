FPSolve (Fixed-Point Solvers)
============================

General-purpose fixed-point computation engines used by analyses such as CLAM
and range analyses.

Overview
--------

FPSolve implements reusable algorithms for solving monotone systems of
equations over abstract domains and lattices.

**Location**: ``lib/Solvers/FPSolve/``

**Main capabilities**:

- Worklist-based and chaotic iteration engines.
- Widening and narrowing operators for convergence acceleration.
- Extensible abstractions for problem definitions and lattices.

Typical Use Cases
-----------------

- Building numerical and pointer analyses as fixed-point computations.
- Implementing abstract interpreters with custom domains.
- Experimenting with different convergence and widening strategies.

Basic Usage (C\+\+)
-------------------

.. code-block:: cpp

   #include <Solvers/FPSolve/FPSolver.h>

   MyProblem Problem(/* ... */);
   FPSolver Solver(Problem);
   auto Solution = Solver.solve();

Features
--------

- **Fixed-point engines** – Chaotic iteration and worklist-based solvers for
  monotone systems.
- **Convergence acceleration** – Support for widening, narrowing, and related
  techniques.
- **Extensible framework** – Abstractions to plug in custom lattices, transfer
  functions, and termination conditions.

Integration Notes
-----------------

FPSolve is used internally by several Lotus analyses and applications that are
implemented as fixed-point computations. See :doc:`solvers` for an overview of
where FPSolve fits in the solver stack.


