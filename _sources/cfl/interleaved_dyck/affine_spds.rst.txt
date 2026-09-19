Affine Synchronized Pushdown Systems
====================================

The ``AffineSPDS`` module strengthens the endpoint-only SPDS upper bound by
comparing joint affine relations between the histories of projected
witnesses. It reuses the SPDS pushdown machinery with an ``AffineSemiring``
weight domain: for a fixed shared map from original graph edges to GF(2)
matrices, it computes the affine hull of each projected witness language and
retains a pair only when the two hulls intersect.

**Location**: ``include/CFL/InterleavedDyck/AffineSPDS/``,
``lib/CFL/InterleavedDyck/AffineSPDS/``

This is an extension of the POPL 2019 synchronized-pushdown construction, not
an algorithm attributed to that paper. The paper supplies the separate
call/field PDS construction and weighted-saturation interface; the affine
history interpretation is this module's own contribution.

Semantics and precision
-----------------------

For a fixed shared map from original graph edges to GF(2) matrices, let
``rho(e1 ... ek) = M_e1 ... M_ek`` and ``rho(epsilon) = I``. For the
call-valid and field-valid trace languages of a query, the engine computes
the affine hulls ``H_call`` and ``H_field`` and retains a pair only if these
spaces intersect. For empty endpoint stacks:

.. code-block:: text

   concrete same-path two-stack reachability
     <= joint affine history synchronization
     <= independent diagonal-block readout
     <= Boolean SPDS endpoint intersection

The implementation computes exact affine hulls for the fixed observer on each
individual projection. It does not retain exact sets of matrices and does not
solve arbitrary same-path two-stack reachability exactly. Intersecting two
nonempty hulls may retain a false positive, so all APIs use ``mayReach`` and
candidate pair sets: false establishes unreachability under the supplied
graph semantics; true is not a concrete witness.

``Verdict`` reports ``MayReach`` when the hulls overlap,
``ProjectionRejected`` when one projection is empty, and ``AffineSeparated``
when both hulls are nonempty and disjoint. In the separated case
``HistoryComparison::certificate`` returns a ``SeparationCertificate``: a
functional that annihilates both direction spaces and evaluates differently
on the two offsets. ``certificate.verify`` checks the equation against the
supplied hulls.

``ComparisonMode`` selects the readout: ``Joint`` uses the full hull
intersection, ``Independent`` projects onto the observer's diagonal blocks,
and ``Projection`` tests endpoint nonemptiness. All modes still compute the
same joint weighted saturation; they are precision ablations, not independent
performance implementations.

Key API surface
---------------

.. list-table:: Public AffineSPDS types
   :header-rows: 1
   :widths: 30 70

   * - Type
     - Purpose
   * - ``Solver``
     - ``prepare(graph)`` builds an automatic observer and compiles the
       analysis; ``prepare(graph, observer)`` accepts a caller-supplied
       ``HistoryObserver``.
   * - ``PreparedAnalysis``
     - ``queryFrom``/``queryTo``, ``analyzeFrom``/``analyzeTo``/``analyzeAll``,
       and ``analyzeDemands``, each parameterized by ``ComparisonMode``.
   * - ``QueryResult``
     - ``compare(vertex)`` returns a lazily cached ``HistoryComparison``;
       ``compareStacks`` queries exact stacks; ``mayReach``,
       ``spdsMayReach``, ``independentMayReach``, ``parenthesisReachable``,
       and ``bracketReachable`` are convenience predicates.
   * - ``HistoryComparison``
     - Both hulls, a verdict, an optional separation certificate, and the
       independent per-block ablation.
   * - ``synchronize(...)``
     - Free function combining queries to two affine-weighted PDS automata.
   * - ``HistoryObserver``
     - Total shared map from original edges to matrices, with ``parity``,
       ``orderedPair``, ``cyclic``, ``directSum``, and ``automatic``
       constructors plus ``write``/``read``.
   * - ``AffineSemiring``
     - Finite-height idempotent semiring: combine is the affine hull of a
       union, extend is the affine hull of products.
   * - ``AffineSpace`` / ``LinearBasis``
     - ``a + span(B)`` hulls with a canonical reduced row-echelon basis over
       GF(2).
   * - ``Matrix`` / ``BitVector`` / ``MatrixLayout``
     - Packed GF(2) arithmetic with no 32/64-dimensional cap.
   * - ``SeparationCertificate`` / ``separate``
     - Separating equation for two nonempty disjoint hulls.

``Options`` carries the observer options, stack acceptance, per-projection
``Limits``, ``max_matrix_dimension``, ``compress_observer``,
``readout_batch_threshold``, and slice-cache bounds. Directional slices are
cached by SCC and direction, bounded by ``max_cached_slices`` and
``max_cached_slice_rules``; either zero disables caching. ``Statistics``
reports saturation counters, matrix and coordinate dimensions, maximum affine
rank, observer microseconds, slice-cache hits, compiled rules, prepared
weights, and ``AlgebraStatistics`` algebra counters.

Observers
---------

``HistoryObserver::automatic`` is a deterministic graph-only heuristic: it
sorts original edges, selects one non-default outgoing edge per branch plus
additional alternatives up to ``max_events`` (default 4), builds 2x2 parity
observers and 3x3 ordered-pair observers up to ``max_order_pairs`` (default
2), and forms their direct sum. The default matrix dimension is at most 14.
No selection yields the 1x1 identity observer. ``parity``, ``orderedPair``,
``cyclic``, and ``directSum`` are additional deterministic constructors.
Unmapped edges carry identity.

Command line
------------

.. code-block:: console

   cmake --build build --target lotus-cfl-interleaved-dyck-affine-spds
   build/bin/lotus-cfl-interleaved-dyck-affine-spds --query 0 4 input.dot
   build/bin/lotus-cfl-interleaved-dyck-affine-spds --all-pairs --mode independent input.dot

The tool requires exactly one query scope: ``--all-pairs``,
``--query SOURCE TARGET``, ``--source V``, ``--target V``, or
``--queries FILE``. ``--direction auto|post|pre`` controls pair and batch
evaluation. ``--call-prefix`` and ``--field-prefix`` allow any call or field
stack at the queried vertex; ``--call-stack IDS`` and ``--field-stack IDS``
query exact comma-separated stacks, top first, with ``--query``.

``--events N`` and ``--order-pairs N`` set the automatic observer budgets
(defaults 4 and 2). ``--identity`` selects the identity observer, equivalent
to Boolean SPDS. ``--observer FILE`` reads a shared original-edge matrix map
and ``--dump-observer FILE`` saves one, including independent-block metadata.
``--mode joint|independent|spds`` selects the comparison. ``--certificate``
includes a separating affine equation when available (joint mode only).
``--json`` emits machine-readable results and statistics. ``--pairs``
includes sorted candidate pairs and ``--timings`` includes instrumented phase
times. ``--vertex V`` adds a vertex. ``--max-dimension N`` caps the matrix
dimension; ``--max-states N``, ``--max-transitions N``, and
``--max-updates N`` bound each projection's saturation.

Exit codes are 0 for a completed computation, 2 for invalid input or I/O, and
3 for a resource failure. Failure cases print no result relation.

Build and test
--------------

.. code-block:: console

   cmake --build build --target interleaved_dyck_affine_spds_test
   ctest --test-dir build -R interleaved_dyck_affine_spds --output-on-failure

See also :doc:`/cfl/interleaved_dyck/spds` and :doc:`/cfl/interleaved_dyck/lcl`.