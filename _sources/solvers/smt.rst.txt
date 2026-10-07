SMT (Satisfiability Modulo Theories)
====================================

SMT solving infrastructure with Z3 integration and theory solvers.

Overview
--------

Lotus provides SMT solving capabilities for constraint checking, invariant
synthesis, symbolic execution, and formula simplification.

**Location**: ``lib/Solvers/SMT/``

Capabilities
------------

- Direct Z3 integration as the primary SMT backend.
- Theory support for bit-vectors, integer and real arithmetic, arrays, and uninterpreted functions.
- Solver abstraction layer (:doc:`libsmt`) providing backend-agnostic formula construction.
- Specialized formula abstraction, normalization, and simplification modules.

Typical Use Cases
-----------------

- Verification condition checking and safety property validation.
- Solving constrained Horn clauses (CHCs) in verification pipelines.
- Path feasibility checking in symbolic execution.
- Symbolic abstraction of bit-vector constraints.

Basic Usage (C\+\+)
-------------------

Analyses in Lotus typically interact with SMT solvers either via direct Z3:

.. code-block:: cpp

   #include <z3++.h>

   z3::context ctx;
   z3::solver solver(ctx);

   z3::expr x = ctx.bv_const("x", 32);
   z3::expr y = ctx.bv_const("y", 32);
   solver.add(x + y == ctx.bv_val(10, 32));

   if (solver.check() == z3::sat) {
     z3::model model = solver.get_model();
     // ...
   }

or via the solver-agnostic abstraction layer :doc:`libsmt`:

.. code-block:: cpp

   #include "Solvers/SMT/LIBSMT/SMTFactory.h"
   #include "Solvers/SMT/LIBSMT/SMTSolver.h"

   auto &factory = SMTFactory::instance();
   auto solver = factory.createSolver();

   auto intSort = factory.getSort("Int");
   auto x = factory.makeVariable(intSort, "x");
   solver->assertExpr(factory.makeGT(x, factory.makeIntVal(0)));
   auto result = solver->check();

Submodules
----------

The ``lib/Solvers/SMT/`` directory contains several specialized libraries:

- :doc:`libsmt` — SMT solver abstraction layer over backend solvers.
- :doc:`symabs` — SMT bit-vector formula abstraction into numerical abstract domains.
- :doc:`staub` — SMT Theory Arbitrage (unbounded to bounded theory transformation).
- :doc:`tuna` — Compiler-optimization-based SMT simplification via LLVM IR.
- :doc:`smtsampler` — Sampling satisfying models from SMT formulas.
- :doc:`smtstabilizer` — SMT-LIB2 normalization library to reduce syntactic variance.
- :doc:`egraphs_simp` — E-graph based quantifier simplification for SMT formulas.

See Also
--------

- :doc:`index` — Overview of solver backends in Lotus
- :doc:`../verification/symabs-ai` — Program-level symbolic abstraction for LLVM IR


