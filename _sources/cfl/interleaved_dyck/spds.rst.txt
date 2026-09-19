Synchronized Pushdown Systems
=============================

The ``SPDS`` module implements the reachability construction of Spath, Ali,
and Bodden, POPL 2019 (Definition 4). It computes a directed, typed upper
bound for interleaved-Dyck reachability by running two independent pushdown
closures: a call projection over parenthesis labels and a field projection
over bracket labels. Each individual projection is exact relative to its
rules and seed regular language; their conjunction is a sound upper bound,
not an exact same-path two-stack reachability procedure.

**Location**: ``include/CFL/InterleavedDyck/SPDS/``,
``lib/CFL/InterleavedDyck/SPDS/``

The module supplies an independently written saturation implementation of
``post*`` and ``pre*``; it does not import the authors' Java implementation
or reproduce their experimental artifact.

Projections and result contract
-------------------------------

For the default graph query, both the initial and final stacks are empty:

.. code-block:: text

   R_call  = {(u,v) | some u->v path has a balanced parenthesis projection}
   R_field = {(u,v) | some u->v path has a balanced bracket projection}
   upper_bound = R_call intersection R_field

A true interleaved-Dyck path witnesses both projections and is never
discarded. A missing pair certifies absence of such a path. A present pair
need not have a single balanced witness: two different graph paths may
witness the two projections (Section 4.1, Figure 7 of the paper).

``Result`` exposes ``upper_bound``, the two independent projection relations
``parenthesis_pairs`` and ``bracket_pairs``, and ``statistics``. ``mayReach``
tests membership in the upper bound. ``Options::parentheses`` and
``Options::brackets`` select ``StackAcceptance::Empty`` (default) or ``Any``
for the queried vertex. ``Any`` permits unmatched opens at the endpoint but
never an underflow or mismatched pop. ``Options::limits`` bounds each
individual PDS saturation; exceeding a limit throws ``ResourceLimit`` and no
partial result is returned.

Key API surface
---------------

.. list-table:: Public SPDS types
   :header-rows: 1
   :widths: 30 70

   * - Type
     - Purpose
   * - ``Solver``
     - ``prepare(graph)`` compiles the two projections once.
   * - ``PreparedAnalysis``
     - ``queryFrom``/``queryTo`` retain the two automata for detailed stack
       queries; ``analyzeFrom``/``analyzeTo``/``analyzeAll`` bulk read out
       endpoint pairs; ``analyzeDemands`` groups requested pairs by the
       smaller number of distinct sources or targets.
   * - ``QueryResult``
     - ``mayReach``, ``parenthesisReachable``, ``bracketReachable``,
       ``mayAccept(vertex, parentheses, brackets)``, and access to the two
       ``Automaton<BooleanSemiring>`` results.
   * - ``PushdownSystem<Domain>``
     - Normal/push/pop PDS rules, wildcard preserve/push rules, and regular
       seed languages.
   * - ``Automaton<Domain>``
     - Queryable ``post*``/``pre*`` result with ``accepts``,
       ``acceptsPrefix``, ``weight``, and ``weightWithPrefix``.
   * - ``SaturationSession<Domain>``
     - Incremental weighted saturation; rules can be added monotonically and
       ``run()`` resumed.
   * - ``RegularSet``
     - Explicit state/transition/final construction for regular seeds.
   * - ``BooleanSemiring`` / ``RelationSemiring``
     - Boolean weights and finite binary-relation typestate weights.
   * - ``SynchronizedSystem<Domain>``
     - Front-end-neutral data-flow builder over variable/statement controls
       with ``postStar``/``preStar``.

``Limits`` bounds automaton states, stored transitions, or successful weight
updates; zero means unlimited. Counts include seed normalization and initial
transitions. ``ResourceLimit`` derives from ``std::runtime_error``.

Synchronized data-flow builder
------------------------------

``SynchronizedSystem`` encodes each data-flow transfer as one rule in a
call-PDS over globally unique variable controls and statement stacks, plus a
field-PDS over ``(variable, statement)`` controls and access-path stacks.
``addNormal``, ``addStore``, ``addLoad``, ``addCall``, and ``addReturn`` each
contribute one transfer. ``SynchronizedResult`` queries synchronized
configurations with ``weight``, ``mayAccept``, ``weightAt``, ``mayAlias``,
and ``mayReachNode``. Clients supply identity flows, kills, caller edges, and
alias-induced transfers explicitly; this is not a complete pointer-analysis
front end.

Command line
------------

.. code-block:: console

   cmake --build build --target lotus-cfl-interleaved-dyck-spds
   build/bin/lotus-cfl-interleaved-dyck-spds --query 0 4 input.dot
   build/bin/lotus-cfl-interleaved-dyck-spds --all-pairs --pairs input.dot

The tool requires exactly one query scope: ``--all-pairs``,
``--query SOURCE TARGET``, ``--source V``, ``--target V``, or
``--queries FILE``. Batch files contain one ``SOURCE TARGET`` pair per line
and may be read from stdin with ``--queries -``. ``--direction auto|post|pre``
controls pair and batch evaluation. ``--call-prefix`` and ``--field-prefix``
allow pending calls or stores at a forward endpoint. ``--vertex V`` adds a
vertex, including isolated vertices. ``--pairs`` prints sorted candidate
pairs and ``--timings`` prints instrumented phase times.
``--max-states N``, ``--max-transitions N``, and ``--max-updates N`` bound
each projection's saturation; zero means unlimited.

Exit codes are 0 for a completed computation (including an unreachable
answer), 2 for invalid input, and 3 for a resource limit. Failure cases print
no result relation.

Build and test
--------------

.. code-block:: console

   cmake --build build --target interleaved_dyck_spds_test
   ctest --test-dir build -R interleaved_dyck_spds --output-on-failure

See also :doc:`/cfl/interleaved_dyck/affine_spds` and :doc:`/cfl/interleaved_dyck/lcl`.