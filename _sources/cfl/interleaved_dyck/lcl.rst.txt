Linear Conjunctive Language Reachability
========================================

The ``LCL`` module implements the interleaved-Dyck instantiation of the
linear-conjunctive-language reachability algorithms of Zhang and Su, POPL 2017
(Algorithms 1 and 2, instantiated with Table 2). It computes an all-pairs
sound upper bound for directed, typed interleaved-Dyck reachability over two
independent typed stacks: parentheses and brackets. Neutral edges denote
epsilon and are eliminated exactly before saturation. No stack-depth or
path-length bound is used.

**Location**: ``include/CFL/InterleavedDyck/LCL/``,
``lib/CFL/InterleavedDyck/LCL/``

The engine consumes the shared ``lotus::cfl::interleaved_dyck::Graph``
directly, without symmetrization or unary projection. It is an independent
C++17 implementation, not a copy of the authors' experimental artifact.

Algorithms
----------

``Algorithm::Baseline``
   Implements Algorithm 1's white-summary saturation.

``Algorithm::Refined``
   Implements Table 2 and Algorithm 2 with monotone white/gray summary facts.
   Boundary-label masks retain all feasible derivations on multigraphs,
   independently of insertion/worklist order. ``Options::enable_feasibility``
   applies the Section 5.3 endpoint filters; it is ignored in baseline mode.

``Options`` defaults to ``Algorithm::Refined`` with feasibility enabled.
``max_summaries`` and ``max_normalized_edges`` default to zero (unlimited).
A positive limit throws ``std::length_error`` when exceeded; a resource-limited
run never returns an unfinished relation as an upper bound. Invalid enum
values and invalid graph label kinds throw ``std::invalid_argument``.

Result contract
---------------

``Solver::analyze`` returns an all-pairs sound upper bound, not an exact
interleaved-Dyck relation:

``Result::upper_bound``
   Presence does NOT certify a balanced witness path. Absence proves
   unreachability in the supplied directed graph. Includes empty paths at
   every vertex and all neutral-only paths.

``Result::mayReach(source, target)``
   Expected O(1) membership test. Vertices absent from the input return
   false.

``Result::statistics``
   Counters for input vertices/edges, normalized edges, epsilon pairs,
   summaries, gray summaries, summary upgrades, left/right term updates,
   rule lookups, worklist pops, and peak worklist size.

The solver is stateless and reentrant; the graph is not modified. For a fixed
alphabet and an epsilon-free graph, expected saturation time is ``O(|V|*|E|)``
and space ``O(|V|^2)``, apart from storing the input. Hash tables give
expected, not worst-case, constant-time membership. With neutral edges the
bound applies to the epsilon-eliminated graph, which can be denser.

Using the Solver
----------------

.. code-block:: cpp

   #include "CFL/InterleavedDyck/LCL/Solver.h"

   using namespace lotus::cfl::interleaved_dyck;

   Graph graph = Graph::parseDotFile("input.dot");
   lcl::Options options;
   options.algorithm = lcl::Algorithm::Refined;
   options.enable_feasibility = true;

   lcl::Result result = lcl::Solver{}.analyze(graph, options);
   bool candidate = result.mayReach(source, target);

Command line
------------

.. code-block:: console

   cmake --build build --target lotus-cfl-interleaved-dyck-lcl
   build/bin/lotus-cfl-interleaved-dyck-lcl --query 0 4 input.dot
   build/bin/lotus-cfl-interleaved-dyck-lcl --print-upper input.dot

``--baseline`` selects Algorithm 1's white-node baseline. ``--no-feasibility``
disables the Section 5.3 endpoint filters. ``--query SOURCE TARGET`` reports
``may-reach`` or ``unreachable``. ``--print-upper`` prints sorted upper-bound
pairs. ``--max-summaries N`` fails above ``N`` summaries and
``--max-normalized-edges N`` caps epsilon expansion; zero means unlimited.
``--`` ends option parsing; ``-h`` and ``--help`` show the usage text.

The tool prints the engine name, semantics, input and normalized edge counts,
epsilon pairs, summary counters, upper-bound pair count, and elapsed time. A
query vertex absent from the input graph is diagnosed as an input error.
Successful analyses return zero regardless of query outcome; input errors and
resource failures return one and do not print a partially computed relation.

The CLI reuses the Core DOT reader. Use one edge per line with labels such as
``op--7``, ``cp--7``, ``ob--9``, ``cb--9``, or ``normal``/``eps``/``epsilon``.

Build and test
--------------

.. code-block:: console

   cmake --build build --target interleaved_dyck_lcl_test
   ctest --test-dir build -R interleaved_dyck_lcl --output-on-failure

See also :doc:`/cfl/interleaved_dyck/spds` and :doc:`/cfl/interleaved_dyck/mcfl`.