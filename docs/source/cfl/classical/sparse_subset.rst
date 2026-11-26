Sparse-Subset CFL Saturation
============================

Paper
-----

Lotus implements the all-pairs saturation construction from *Beating
Rytter's Bound: All-Pairs CFL Reachability in
:math:`O(n^3 (\log\log n)^2/\log^2 n)` Time*.

The implementation is self-contained. It uses the Lotus ``Grammar``,
``LabeledGraph``, and ``Relation`` interfaces and does not depend on an
external artifact.

Problem and key idea
--------------------

A worklist solver derives one labeled fact at a time, so a production such as
``C -> A B`` re-derives many identical pivot combinations whenever several
``B`` facts leave the same node. The paper instead solves each binary
production as an **incremental set-pair** problem:

* one family of node sets per production endpoint, maintained for the
  ``B``-side (rows) and for the ``C``-side (columns), and
* an exact reporting primitive that returns a whole set difference for a
  probe pair instead of enumerating single pairs.

For a production ``C -> A B`` the engine keeps two growing families and
answers the two orientations of the paper's report:

.. code-block:: text

   B(u, v) fact  ->  report R_C[v, :] \ R_A[u, :]
   C(v, x) fact  ->  report R_B[:, v] \ R_A[:, x]

A report is complete before the facts it returns are inserted, so every
realization of the paper's invariants sees the same Y family it was counted
against.

Three primitives
----------------

``BitPlaneCounters``
   Stores exact unsigned lane counters in bit planes and implements lane-wise
   vector addition with bit-sliced full-adder carry propagation, so a carry
   cannot leak into a neighbouring lane. It is used both for sparse-subset
   vectors and for the two orientation-specific intersection-count arrays.
   Exceeding the declared per-lane width raises ``std::overflow_error``
   rather than silently saturating.

``SparseSubsetTable``
   Stores ``V_S`` for every non-empty subset of at most ``s`` coordinates in
   each length-``L`` block of a monotonically growing Boolean matrix. A
   coordinate incidence list updates exactly those stored vectors that
   contain a newly set entry, so a 0 to 1 update never rescans the matrix. A
   query splits its sorted, duplicate-free coordinate batch at block
   boundaries and into pieces of at most ``s`` coordinates, then adds the
   precomputed vectors; each returned vector is exact, not Booleanized.

``TwoSidedSetPair``
   Maintains both growing families, their snapshots, pending lists, the
   sparse tables for the two snapshot matrices, ``c_X``, ``I_col``, and
   ``I_row``. Promotion runs in both directions once a pending list reaches
   the threshold ``D``; ``Report`` and ``Count`` then combine the group counters
   with the exact pending-list corrections for the facts not yet promoted.
   Groups are aligned to sparse-table blocks using the paper's
   padded-universe convention, so every block belongs to exactly one report
   group.

Parameters
----------

For a caller-supplied ``epsilon`` in ``(0, 1)`` the engine uses

.. code-block:: text

   lambda = ceil(log2 n)              D = ceil(n / lambda^2)
   g      = lambda                    eta = epsilon / 4
   s      = max(1, floor(eta log2(n) / log2(e ceil(n/D))))
   L      = s ceil(n/D)

and pads the internal universe to a multiple of ``g L``. The pathological
small-``n`` cases keep the block/group alignment invariant by clamping ``s``,
``L``, and ``D`` below. ``SolverSession`` fixes ``epsilon = 0.25``; direct
callers of ``engines::sparse_subset::solveAllPairs`` may pass another value and
get ``std::invalid_argument`` for anything outside ``(0, 1)``.

Lotus implementation
--------------------

Public API
   ``include/CFL/Classical/Solvers/Engines/SparseSubset/SparseSubsetCFL.h``
   (entry point) and ``SparseSubsetTabulation.h`` (the three primitives).

Algorithm
   ``lib/CFL/Classical/Solvers/Engines/SparseSubset/SparseSubsetCFL.cpp``.

Integration
   ``SolverBackend`` value parsed from ``--solver sparse-subset`` in the
   classical, alias, and value-flow drivers. The backend occupies a private
   enum value next to the last public one, so
   ``CFL/Classical/Solvers/SolverSession.h`` is unchanged;
   ``solverBackendName`` reports ``"sparse-subset"``.

``solveAllPairs`` validates the grammar, creates row and column
``TwoSidedSetPair`` instances for every binary production, and seeds the
terminal edges followed by the nullable self-facts. New facts are forwarded to
the affected instances at insertion time, before they are queued, so a report
always sees the facts that were already inserted. Unary productions are closed
on the worklist. The returned ``Relation`` contains every terminal and
nonterminal fact.

Backend contract
----------------

The engine is a **batch all-pairs** backend.

* A session recomputes the whole fixed point after each graph update. It does
  not reuse a previous relation incrementally, so ``addTerminalEdge`` /
  ``addNode`` followed by ``solve`` cost a full saturation. A ``solve`` with no
  pending change returns the previous snapshots and zero work counters without
  dispatching the engine.
* The result is the complete grammar-relative relation. ``--result-scope``
  filtering and the POCR ``Insert``/``Follow`` projection do not restrict what
  the engine computes, so ``--unidirectional`` does not change its answers.
* ``processed_items`` reports the number of facts in the recomputed relation;
  ``added_edges`` is the delta against the relation it replaced.

Validation
----------

``tests/unit/CFL/Classical/SparseSubsetEngineTest.cpp`` covers the three
primitives directly, including overflow and misuse diagnostics, a seeded
randomized differential test of ``report``/``count``/``subset`` against a
naive set model, and a full-relation comparison against the
``sparse-bitvector`` backend for closure, nullable seeds, isolated nodes, and
incremental updates. ``tests/regress/CFL/Classical`` runs the CLI drivers with
``--solver sparse-subset`` and compares the relation size against the
reference backends.
