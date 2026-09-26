CERT-CFL: Cardinality-Certified Exact Solving
=============================================

CERT-CFL is an exact all-symbol CFL-reachability solver proposed by the Lotus
team. It uses universal-degree certificates, symbolic nullable identity, a
finite threshold domain, and partition refinement to promote dense relation
blocks to exactness without enumerating every pair. No external paper is
associated with the algorithm; see the source headers for the precise
promotion rules.

Problem and key idea
--------------------

The kernel works on normalized epsilon, unary, and binary productions plus
seed facts (terminal or nonterminal edges). Nodes and symbols are dense,
zero-based IDs. Instead of a concrete triple worklist, CERT-CFL partitions
the nodes into blocks and maintains, for every symbol and block pair, a
**tile** that summarizes the relation between the two blocks:

``may``
   The tile contains at least one derivation that uses a seed edge.

``out`` / ``in``
   Universal minimum outgoing/incoming degree bounds over the block, i.e.
   lower bounds valid for every member node, not averages.

A tile is promoted to exact (``FULL``) once its universal degree bounds prove
that it contains every node pair of the target block. The promotion condition
for ``A -> B C`` with a shared pivot block ``K`` is the strict inequality

.. code-block:: cpp

   const bool overlap = left.out > middle_size - right.in;

i.e. ``left.out + right.in > |K|``, written to avoid unsigned overflow.
Multiple proofs combine using maximum, not addition. Nullable identity
participates in these lower bounds but never sets the ``may`` bits, which
permits exact ``IDENTITY`` tiles without refining them into singleton
diagonals.

Refinement
----------

The default finite threshold domain is ``{0, 1, floor(t/2)+1, t}`` with
rounding downward. Blocks that cannot be certified at the current granularity
are bisected, and saturation continues on the finer partition. Balanced block
bisection eventually reaches singleton resolution, which is always exact.
Recursive transfer uses copies of premise tiles so an output tile may alias an
input tile.

The implementation stores the four threshold values as compact states and
indexes only active neighboring tiles for binary joins. If a sufficiently fine
level makes no ``FULL`` promotion, it jumps directly to the exact singleton
level instead of replaying the same sparse derivations at every intervening
power-of-two partition. Singleton certificates use a packed fact/index layout;
this is the terminal CERT refinement level.

Resource limits
---------------

``Options`` bounds the dense work:

``max_tiles``
   A count of active tile records, not a process-memory cap. Zero (the
   default) is unlimited. Large logical tile spaces switch from a dense array
   to a sparse tile table because most block pairs are empty.

``max_levels`` / ``max_dense_joins``
   Bound refinement depth and cumulative dense joins; zero means unlimited.

A configured limit raises ``ResourceLimit``. ``observed = nullopt`` resolves
all symbols; an empty observed vector means no required outputs.

Querying results
----------------

``Result`` is a mutable monotone relation. ``extend`` adds node and seed deltas
in place and saturates only their consequences.

``answer(symbol, u, v)``
   Returns ``std::optional<bool>``; ``nullopt`` is **unknown**, never false.

``contains(symbol, u, v)``
   Throws if the requested pair remains unresolved.

Exact whole-symbol traversal and counts throw ``logic_error`` for an
unresolved symbol, while individual pairs may still be answered definitively.
Counts for resolved symbols are cached; the off-diagonal union counter handles
overlap across symbols without expanding ``FULL`` tiles into pair sets.

Lotus implementation
--------------------

Public API
   ``include/CFL/Classical/Solvers/Engines/CERT/CertCFL.h`` (dependency-free
   kernel) and ``CertCFLEngine.h`` (``Relation`` adapter).

Algorithm
   ``lib/CFL/Classical/Solvers/Engines/CERT/CertCFL.cpp`` and
   ``CertCFLEngine.cpp``.

Integration
   ``SolverBackend::CertCFL`` and ``--solver cert`` in the classical,
   alias, and value-flow drivers.

The adapter resolves **all grammar symbols** because ``Relation`` supports
arbitrary-label membership, visitors, and total counts; setting
``options.cert_cfl.observed`` is rejected. ``unidirectional=true`` is also
rejected: CERT-CFL does not implement restricted POCR Insert/Follow
evaluation. Seed-only symbols (no unary/binary defining rule) are kept as
exact sparse inputs plus symbolic identity, so they do not force singleton
refinement.

``add`` buffers monotone seed insertions. ``solve`` extends the existing CERT
relation in place with only the new seed/node delta. A compressed result is
materialized once into singleton indexes before its first update; subsequent
rounds retain and extend those indexes. Mutation is single-threaded; do not
query, update, or solve from a traversal callback while saturation is running.

Validation
----------

The native ``CertCFLEngineTest`` compares complete labeled relations against
the ``SparseSet`` reference backend, exercises monotone incremental rounds and
nullable nodes, checks hard resource limits and the off-diagonal Count-symbol
union, and confirms the rejection of unsupported modes and external graph
mutation. The standalone kernel was additionally differential-tested against
an independent Boolean fixed-point reference.
