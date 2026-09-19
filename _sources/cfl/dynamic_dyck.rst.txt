Dynamic Bidirected Dyck Reachability
====================================

The module provides two independent algorithms: the existing POPL 2022
``WeightedQuotientSolver`` and the POPL 2024 ``PrimaryComponentSolver`` described below. Both link through
``CanaryDynamicDyck``; existing callers and the original CLI are unchanged.

``WeightedQuotientSolver`` directly ports the C++ dynamic algorithms from Li,
Satya, and Zhang's `Efficient Algorithms for Dynamic Bidirected
Dyck-Reachability <https://doi.org/10.1145/3498724>`_ (POPL 2022).

**Headers**: ``include/CFL/DynamicDyck/``

**Implementation**: ``lib/CFL/DynamicDyck/``

**Namespace**: ``lotus::cfl::dynamic_dyck``

Shared graph, file IO, and statistics headers live at the module root.
The public solver headers live in their respective algorithm directories:
``WeightedQuotient/WeightedQuotientSolver.h`` and
``PrimaryComponent/PrimaryComponentSolver.h``. Their source files have the
same relative paths under ``lib/CFL/DynamicDyck/``, with a ``.cpp`` extension.
Shared ``Graph.cpp`` and the original file-workflow adapter
``IO.cpp`` remain at the implementation root. Each algorithm owns its sources
and CMake source list: ``WeightedQuotient/`` contains the original solver,
weighted engine, cycle-deletion correction and artifact DOT parser;
``PrimaryComponent/`` contains the primary-component solver and its two
connectivity backends. Internal headers mirror those directories and use the
``weighted_quotient`` and ``primary_component`` namespaces. The
``CanaryDynamicDyck`` target is unchanged. Template/container support
remains header-only.

The original degree-based merging, fully compressed disjoint sets, indexed
linked worklists, weighted quotient updates, and acyclic recursive splitting
are retained. Original global state belongs to a solver instance. Bug fixes
reset preprocessing state, correct duplicate/missing-edge degree accounting,
and avoid invalid arrow parsing. The `2024 cycle correction
<https://arxiv.org/html/2401.03570v1>`_ motivates a conservative deletion
fallback: if the old affected quotient contains cycles, preprocess the
remaining original graph again. No original-paper update-time bound is claimed.

Library API
-----------

``WeightedQuotientSolver`` supports ``addVertex``, ``insertEdge``, ``deleteEdge``, ``apply``,
``connected``, ``representative``, ``components``, ``graph``, and
``statistics``. Each edge denotes an opening arc and its closing reverse;
updates operate on the pair. Duplicate insertion and absent deletion are
no-ops. Numeric vertex IDs are signed 64-bit integers; labels are unsigned
integers. Insertion creates endpoints; deletion does not. Queries involving
unknown vertices return false. Concurrent access requires synchronization
because representative queries compress paths.

.. code-block:: cpp

   #include "CFL/DynamicDyck/WeightedQuotient/WeightedQuotientSolver.h"
   using namespace lotus::cfl::dynamic_dyck;
   WeightedQuotientSolver solver;
   solver.insertEdge({10, 30, 0});
   solver.insertEdge({20, 30, 0});
   assert(solver.connected(10, 20));
   solver.deleteEdge({30, 20, 0, Parenthesis::Close});
   assert(!solver.connected(10, 20));

``IO.h`` exposes ``runFiles`` for the original opaque string-ID workflow,
returning original timing results and final partitions. ``Graph.h`` offers
stricter numeric convenience parsers separately. Only one Dyck language is
supported; neutral and interleaved bracket constraints are outside this module.

POPL 2024 algorithm
-------------------

``PrimaryComponentSolver.h`` provides DynamicAlgo from Krishna, Lal, Pavlogiannis and
Tuppe, *On-the-Fly Static Analysis via Dynamic Bidirected Dyck Reachability*
(POPL 2024). This implementation was integrated from the user-supplied
``lotus-dynamic-dyck-popl24-v2`` package; it is not an official authors' artifact.
It maintains DSCCs, primary components, edge counts and sparse summaries.
Deletion refines affected components and re-saturates summaries without a
whole-input Dyck-reachability rebuild.

The default edge convention is **reference counted**, unlike ``WeightedQuotientSolver``.
Each insertion, including a complementary reverse record, adds one reference;
deletion removes one reference. Choose ``PrimaryComponentEdgeSemantics::Set`` to match
the existing solver's duplicate and deletion behavior. Both conventions
create missing endpoints on insertion and retain vertices after deletion.
``graph()`` exports unique opening pairs; use ``edgeCounts()`` to retain counts.

.. code-block:: cpp

   #include "CFL/DynamicDyck/PrimaryComponent/PrimaryComponentSolver.h"
   using namespace lotus::cfl::dynamic_dyck;
   PrimaryComponentSolver solver(PrimaryComponentEdgeSemantics::Set);
   solver.insertEdge({10, 30, 7});
   solver.insertEdge({20, 30, 7});
   auto first = solver.vertexIndex(10);
   auto second = solver.vertexIndex(20);
   assert(solver.connectedByIndex(first, second));
   solver.deleteEdge({30, 20, 7, Parenthesis::Close});
   assert(!solver.connectedByIndex(first, second));

The default ``Deterministic`` connectivity backend uses a strong-certificate
hierarchy. The package's implementation-level complexity argument gives
O(n alpha(n)) worst-case update work for a constant alphabet in its stated
RAM model; this is not a machine-checked proof or hard-real-time guarantee.
It is not the exact Eppstein backend from the paper. Select
``PrimaryComponentConnectivityBackend::HDT`` as the practical alternative, with different
amortized/treap cost qualifications. Neither backend is claimed uniformly faster.
External-ID queries require O(log n) lookup; stable dense-index queries are O(1)
with the deterministic backend. Graph construction inserts edges individually,
not through an optimal bulk initializer.

Unknown-ID connectivity queries return false; representative and index lookups
throw for unknown vertices. Instances are independent, movable and noncopyable;
access to one instance requires synchronization. Interrupted mutations fail
closed: subsequent queries throw, and the instance must be replaced. There is
no transactional rollback. ``validate()`` is an expensive debug check.

The supplied implementation package documents its algorithm mapping,
complexity qualifications, provenance and MIT license.

.. code-block:: bash

   cmake --build build --target lotus-cfl-dynamic-dyck \
       dynamic_dyck_primary_component_test dynamic_dyck_primary_component_integration_test \
       dynamic_dyck_primary_component_phases dynamic_dyck_primary_component_allocation_failure \
       dynamic_dyck_primary_component_sparsification
   build/bin/lotus-cfl-dynamic-dyck --algorithm primary-component --stats initial.dot updates.seq
   build/bin/lotus-cfl-dynamic-dyck --algorithm primary-component --backend hdt --counted \
       --print-components initial.dot updates.seq
   ctest --test-dir build -R dynamic_dyck --output-on-failure

The existing CLI exposes POPL 2024 with ``--algorithm primary-component`` and accepts the
same opaque node IDs and type suffixes. It defaults to **set semantics** for
compatibility; ``--counted`` enables reference counts. ``--backend`` selects
``deterministic`` (default) or ``hdt``. Sequence endpoints are preallocated and
timing covers update calls only, excluding parsing and initial saturation.
The original timing-only output format is preserved; ``--stats`` identifies
the selected algorithm, backend and semantics. POPL 2024 rejects
``0``/``--recompute``. Without ``--algorithm`` the original solver and its modes
remain unchanged. There are no separate POPL 2024 tool executables.
Tests include a Boolean Dyck-grammar oracle, primary-connectivity BFS,
pre-fixpoint phase invariants, allocation failures, parser checks and
set-mode comparisons against the existing solver. Instrumented phase hooks
are compiled into a separate test-only library, never the production target.

Original benchmark workflow
---------------------------

.. code-block:: bash

   cmake --build build --target lotus-cfl-dynamic-dyck dynamic_dyck_test
   build/bin/lotus-cfl-dynamic-dyck 1 initial.dot updates.seq
   build/bin/lotus-cfl-dynamic-dyck 0 initial.dot updates.seq
   ctest --test-dir build -R dynamic_dyck --output-on-failure

``0`` selects recomputation after every input record, including no-ops;
``1`` selects dynamic updates. The CLI accepts original
``SOURCE->TARGET[label="op--TYPE"]`` / ``cp--TYPE`` graph records and
``A|D SOURCE TARGET LABEL`` sequence records, interning names and suffixes
as strings. Default stdout is one elapsed-seconds value and a trailing space,
with no newline, matching the original table scripts. Timing retains the
original scopes; ``--stats`` and ``--print-components`` add diagnostics.

Run the original table scripts directly; paths are resolved relative to the
repository, with ``DYCK_REACH_BINARY`` and ``DYCK_BENCHMARK_ROOT`` overrides:

.. code-block:: bash

   bash scripts/cfl/dynamic-dyck/gen_table3.sh
   bash scripts/cfl/dynamic-dyck/gen_table4.sh

The original C++ benchmark inputs live in
``benchmarks/real-world/CFL/DynamicDyck`` and the scripts in
``scripts/cfl/dynamic-dyck``. Unused backup snapshots are omitted.
The optional ``run_benchmarks.py`` runner
supports case selection, dry-run input validation, and partition comparison
without changing the original table ordering; ``--binary`` selects another
build. DDlog is not migrated.
