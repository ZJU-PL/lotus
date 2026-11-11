Integer Linear Programming Reachability
=======================================

The ``ILP`` module implements the demand-driven, single-source/single-target
interleaved-Dyck reachability approximation of Yuanbo Li, Qirun Zhang, and
Thomas Reps, *Single-Source-Single-Target Interleaved-Dyck Reachability via
Integer Linear Programming*, POPL 2023.

**Location**: ``include/CFL/InterleavedDyck/ILP/``,
``lib/CFL/InterleavedDyck/ILP/``

**Headers**: ``include/CFL/InterleavedDyck/ILP/Solver.h``

The engine consumes the shared ``lotus::cfl::interleaved_dyck::Graph`` directly
and answers directed reachability queries between a specified source and target
vertex using an integer linear programming (ILP) backend. Z3 is the default
backend.

Overview
--------

Interleaved-Dyck reachability is undecidable in general. The POPL 2023 approach
over-approximates reachability by generating a regular path expression between
the query endpoints and checking whether an integer linear programming (ILP)
encoding of parenthesis counts has a non-negative, balanced solution.

The implementation consists of:

- **Path expression generation**: Reuses Tarjan's (1981) path expression
  algorithm via ``PathExpressionComputer``.
- **Hierarchical compact acyclic graph (HCAG)**: Terminal, concatenation,
  alternative, and Kleene-star nodes with structural hash-consing in
  ``RegexBuilder``.
- **Standard form conversion**: Normalizes path expressions into standard form
  (Theorem 4.2) by distributing concatenation over alternatives and rewriting
  ``(a|b)*`` as ``(a*b*)*``.
- **Count table encoding**: Encodes the bottom-up count table (Algorithm 2)
  over a postorder DAG traversal into an integer linear program.
- **Cycle traversal variables**: Fresh integer variables for each star,
  shared across the parenthesis dimensions contributed by that cycle.
  Alpha-renaming ensures variable independence when combining concatenation
  children.
- **LCL pruning (Section 4.4)**: Prunes subexpressions that cannot contain an
  InterDyck word using the LCL engine's summary relation over derived epsilon-NFAs.

Result contract
---------------

``Solver::analyze`` returns a ``Result``:

``may_reach == false``
   Certifies that the query endpoints are unreachable under the paper's
   over-approximation.

``may_reach == true``
   Indicates a feasible candidate. It does **not** certify a concrete
   balanced-path witness, because parenthesis ordering across distinct cycles
   is only partially enforced by LCL pruning.

``statistics``
   Provides counters for path expression nodes, ILP disjuncts, rejected
   subexpressions, integer variables, and integer constraints.

Options
-------

Configured through ``lotus::cfl::interleaved_dyck::ilp::Options``:

* ``backend`` (default: ``Backend::Z3``) -- Select the integer-linear solver.
  ``Backend::Gurobi`` is available only when Lotus is configured with
  ``-DLOTUS_ENABLE_GUROBI=ON`` and Gurobi is installed. ``backendAvailable``
  reports whether a backend is present in the current build.
* ``enable_lcl_refinement`` (default: ``true``) -- Use LCL trellis summaries to
  prune dead subexpressions.
* ``allow_unmatched_opens`` (default: ``false``) -- Allow non-negative final
  counts instead of exact equality to zero, accepting paths with unmatched
  openers.
* ``max_disjuncts`` (default: ``0`` / unlimited) -- Bound the number of
  disjuncts during standard-form expansion. Throws ``std::length_error`` when
  exceeded.
* ``max_expression_nodes`` (default: ``0`` / unlimited) -- Bound the number of
  retained path-expression nodes. Throws ``std::length_error`` when exceeded.

Using the Solver
----------------

.. code-block:: cpp

   #include "CFL/InterleavedDyck/ILP/Solver.h"

   namespace dyck = lotus::cfl::interleaved_dyck;

   dyck::Graph graph;
   graph.addEdge(0, 1, dyck::Label::openParenthesis(0));
   graph.addEdge(1, 2, dyck::Label::closeParenthesis(0));

   dyck::ilp::Options options;
   options.enable_lcl_refinement = true;

   dyck::ilp::Result result = dyck::ilp::Solver{}.analyze(graph, 0, 2, options);
   if (result.may_reach) {
     // Candidate reachable path exists
   }

Build and test
--------------

.. code-block:: bash

   cmake --build build --target cfl_tests
   ctest --test-dir build -R 'cfl_tests.InterleavedDyckILP' --output-on-failure

The optional Gurobi backend is off by default and is not required by CI:

.. code-block:: bash

   cmake -S . -B build-gurobi -DLOTUS_ENABLE_GUROBI=ON
   cmake --build build-gurobi --target lotus-cfl-interleaved-dyck

Set ``GUROBI_HOME`` to the Gurobi installation. Users are responsible for
obtaining a Gurobi commercial or academic license.

See also
--------

* :doc:`lcl` -- POPL 2017 linear-conjunctive-language reachability
* :doc:`spds` -- POPL 2019 synchronized pushdown systems reachability
* :doc:`staged_bounds` -- Staged bounds reachability
