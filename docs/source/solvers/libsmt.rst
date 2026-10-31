LIBSMT — SMT Solver Abstraction Layer
=====================================

A unified abstraction layer over multiple SMT solver backends.

**Headers**: ``include/Solvers/SMT/LIBSMT/``

**Implementation**: ``lib/Solvers/SMT/LIBSMT/``

**Build target**: ``CanarySMT``

Overview
--------

LIBSMT provides a solver-agnostic interface for SMT (Satisfiability Modulo
Theories) solving. It abstracts over solver-specific details (Z3, etc.)
so that Lotus analyses can issue SMT queries without depending on any single
solver backend.

Components
----------

SMTSolver
~~~~~~~~~

Solver wrapper over the Z3 backend:

- ``add(SMTExpr)`` / ``addAll(SMTExprVec)`` — Add a formula to the solver
  context.
- ``push()`` / ``pop()`` — Assertion stack management.
- ``check()`` — Check satisfiability of the current context, returning an
  ``SMTResultType`` (``SMTRT_Sat``, ``SMTRT_Unsat``, ``SMTRT_Unknown``, or
  ``SMTRT_Uncheck``).
- ``getSMTModel()`` — Retrieve a satisfying model as an ``SMTModel``.
- ``reset()`` — Clear all assertions.

SMTExpr and SMTObject
~~~~~~~~~~~~~~~~~~~~~

Expression tree representation used to build formulas:

- ``SMTExpr`` — Typed expression with a solver-specific handle.
- ``SMTObject`` — Base class for all SMT entities (sorts, functions, variables).

SMTFactory
~~~~~~~~~~

Factory for creating SMT constants, values, and expressions. A factory is a
regular object; expressions it creates belong to its Z3 context:

.. code-block:: cpp

   #include "Solvers/SMT/LIBSMT/SMTFactory.h"

   SMTFactory factory;

   SMTExpr x = factory.createBitVecConst("x", 32);
   SMTExpr y = factory.createBitVecConst("y", 32);
   SMTExpr expr = x == y;

   // Scalar and boolean constants are created the same way.
   SMTExpr n = factory.createIntVal(0);
   SMTExpr b = factory.createBoolConst("b");

Z3Expr and Z3Plus
~~~~~~~~~~~~~~~~~

Z3-specific expression handling and extended operations (quantifier support,
optimisation queries, model parsing).

CNF and SATSolver
~~~~~~~~~~~~~~~~~

CNF formula representation and SAT solver interface for boolean-level
reasoning:

- ``CNF`` — Conjunctive normal form representation.
- ``SATSolver`` — SAT solver wrapper (useful for eager bit-blasting).

SMTModel
~~~~~~~~

Represents a satisfying assignment from a solver query. ``Eval`` returns the
value of an expression under the model, while ``getIntValue`` and
``getBoolValue`` return ``(value, is_known)`` pairs. ``getPartialModel``
restricts a model to a chosen set of variables.

SMTConfigure
~~~~~~~~~~~~

Configuration and option management for solver backends.

Usage
-----

.. code-block:: cpp

   #include "Solvers/SMT/LIBSMT/SMTFactory.h"
   #include "Solvers/SMT/LIBSMT/SMTSolver.h"
   #include "Solvers/SMT/LIBSMT/SMTModel.h"

   SMTFactory factory;
   SMTSolver solver = factory.createSMTSolver();

   SMTExpr x    = factory.createBitVecConst("x", 32);
   SMTExpr zero = factory.createBitVecVal(0, 32);

   solver.add(x > zero);
   if (solver.check() == SMTSolver::SMTRT_Sat) {
     SMTModel model = solver.getSMTModel();
     auto value = model.getIntValue(x);   // (value, is_known)
     // ...
   }

See Also
--------

- :doc:`./smt` — SMT solver backend (Z3)
- :doc:`./cudd` — BDD solver backend (CUDD)
- :doc:`./wpds` — Weighted pushdown systems
