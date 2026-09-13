Exact Unary Interleaved Dyck
============================

The ``Unary`` module contains exact algorithms for bidirected unary
``D1``-interleaved-``D1`` reachability. The module name identifies the problem
scope; ``AdaptiveSolver`` and ``FixedCounterSolver`` identify the algorithms.

**Location**: ``include/CFL/InterleavedDyck/Unary/``,
``lib/CFL/InterleavedDyck/Unary/``

Algorithms
----------

``AdaptiveSolver``
   Uses the adaptive two-arm construction. ``solve`` applies the shared
   quotient, decomposes it into weak components, and chooses ``K = 6 * n``
   independently for each component of size ``n``. Components are processed
   sequentially using quadratic finite control. ``solveShallow(graph, K)``
   solves exactly inside ``min(counter1, counter2) <= K`` and preserves the
   caller's K in every weak component without quotienting.

``FixedCounterSolver``
   Implements Kjelstrøm and Pavlogiannis, POPL 2022 Algorithm 1. For ``n``
   vertices in a processed weak component it stores one counter in states ``(v,j)`` for
   ``0 <= j <= 18*n^2 + 6*n`` and leaves the other as a bidirected
   one-counter problem. It is the primary exact baseline for Adaptive.

Both algorithms share typed graph parsing, unary projection, input policy,
quotient sparsification, and the bidirected-Dyck component backend.
Singletons bypass lifted construction. Single-counter components use the
ordinary Dyck backend directly, also for shallow queries: the unused counter
stays zero.

Construction and performance
----------------------------

Sparsification belongs to preprocessing. On a unary input of ``N`` vertices
and ``M`` arcs, its fixed two-label alphabet gives amortized
``O((N+M) alpha(N))`` time and ``O(N+M)`` space. Hash-based unary projection
before it takes expected ``O(N+M)`` time. A quotient of ``q`` vertices has at
most ``4q`` arcs: two possible closing targets per component, plus opening
reverses. The implementation reuses the backend's dense mapping and final
functional closing lists, avoiding duplicate ID remapping and another scan
of the original graph. Adaptive reuses those lists for its vertical parent
map as well. Input reading itself requires ``Omega(N+M)`` work.

The solvers generate lifted transitions directly into the backend. Each
undirected epsilon connection is emitted once and contracted before allocating
closing-edge tables. The tables are indexed by compact epsilon components,
not all lifted states. Final Dyck component IDs are dense; closing transitions
that repeat the last target of a source/label list do not allocate pool entries.

Adaptive releases vertical state maps and parent data after labeling, releases
sorting scratch before allocating the merge DSU, and constructs the horizontal
arm only afterward. Counting-sort ranges use the height bound and compact
component count. Both algorithms identify only the queried zero-state roots
when producing their final vertex partitions.

For quotient weak-component sizes ``n_i``, Adaptive's finite control is
``O(sum n_i^2)`` and its running time is near-quadratic with union-find factors,
plus preprocessing. FixedCounter retains ``O(sum n_i^3)`` finite control for
mixed-counter components. Sequential workspace depends on the largest
component. Without sparsification, Adaptive on ``n`` vertices and ``m`` arcs
has construction cost ``O((n+m)(K+1))`` up to union-find factors, so
``--direct`` with ``K=6n`` can still take cubic time on dense input.

``stats().execution`` provides phase microseconds, weak-component sizes,
fast-path counts and estimated peak construction payload. Adaptive also reports
vertical, horizontal and merge times. Backend counters distinguish generated,
stored and scanned closing edges and compact epsilon-component counts.
State/arc counters sum actual mixed-component work, while full-solve bounds
are maxima across those components; shallow queries report the supplied K.
Payload estimates exclude input/component graphs, final output maps and
allocator overhead. ``--stats`` additionally reports process peak RSS on
macOS/Linux, including parsing and allocator retention, rather than presenting
that high-water mark as per-phase memory.

Exactness and directed input
----------------------------

Every projected arc must have its complement-labeled reverse. Parenthesis IDs
project to counter 1 and bracket IDs to counter 2. Exactness concerns balanced
paths between zero-counter configurations; it does not extend to multi-type
``D_k``-interleaved-``D_k``.

Both option types default to ``RequireBidirected``. The alternative
``AddMissingReverseEdges`` is exact for the resulting symmetrized graph and a
sound overapproximation for the original directed graph. The parser itself
never adds reverse edges.

.. code-block:: cpp

   #include "CFL/InterleavedDyck/Core/Graph.h"
   #include "CFL/InterleavedDyck/Unary/Solver.h"

   using namespace lotus::cfl;

   interleaved_dyck::Graph graph =
       interleaved_dyck::Graph::parseDotFile("bidirected.dot");
   auto adaptive = interleaved_dyck::unary::AdaptiveSolver{}.solve(graph);
   auto fixed = interleaved_dyck::unary::FixedCounterSolver{}.solve(graph);

Command line
------------

.. code-block:: console

   cmake --build build --target lotus-cfl-interleaved-dyck-unary
   build/bin/lotus-cfl-interleaved-dyck-unary --algorithm adaptive --stats bidirected.dot
   build/bin/lotus-cfl-interleaved-dyck-unary --algorithm fixed-counter --stats bidirected.dot

``--direct`` disables shared quotient sparsification. ``--shallow K`` applies
only to Adaptive. ``--bidirect`` explicitly selects symmetrization, and output
states the selected algorithm and resulting guarantee.
``--print-pairs`` groups vertices by component and streams non-reflexive pairs
in ``O(|V| + output)`` time; it does not scan every unrelated vertex pair.

The existing ``benchmarks/real-world/CFL/InterleavedDyck`` corpus is directed,
so exact runs require a genuinely bidirected corpus. Using ``--bidirect`` on
that corpus must be reported as an overapproximate experiment.

Build and test
--------------

.. code-block:: console

   cmake --build build --target interleaved_dyck_unary_test
   ctest --test-dir build -R interleaved_dyck_unary_test --output-on-failure
