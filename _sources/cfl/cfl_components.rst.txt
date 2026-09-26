CFL Reachability Components
===========================

Advanced CFL reachability algorithms and graph analysis frameworks.

Classical CFL Reachability
--------------------------

Grammar-driven CFL reachability utilities and solver backends

**Location**: ``include/CFL/Classical/``, ``lib/CFL/Classical/``. Both trees
use ``Core/``, ``Solvers/Engines/`` (including ``PEARL/``, ``POCR/``,
``SQID/``, ``STG/``, ``Skewed/``, ``EndpointQuotient/``, ``CAT/``,
``IEOCE/``, and ``CERT/``), ``Solvers/Preprocessing/``, ``Clients/Alias/``,
and ``Clients/ValueFlow/``.
The two client implementations consistently use ``AliasClient.*`` and
``ValueFlowClient.*`` filenames.

**Features**:

* One canonical grammar parser with EBNF expansion and binary normalization
* Labeled graph construction for text, DOT, PAG, and PEG-style encodings
* Sparse-set, sparse-bitvector, Graspan epoch/delta, transitive-specialized,
  POCR/hierarchical-POCR, and fully ordered edge-critical-graph backends
* PEARL multi-derivation, Sqid relation chaining, Stg staged solving, and
  CERT-CFL cardinality-certified solving
* Native POCR grammar/graph input, unidirectional summarization, client graph
  simplification, and RSM-guided foldability checking
* Incremental solver sessions for dynamically discovered terminal edges
* Adapters for Lotus AserPTA constraint graphs and Lotus SVFG value flow
* End-to-end LLVM bitcode alias analysis with CFL-driven indirect calls
* End-to-end context-sensitive LLVM value-flow analysis over Lotus SVFGs
* Strong-update-aware CFL preparation for Lotus SVFGs

See :doc:`/cfl/classical/pearl`, :doc:`/cfl/classical/stg`, and
:doc:`/cfl/classical/sqid` for paper references, key ideas, algorithm
walkthroughs, and exact source mappings.
See :doc:`/cfl/classical/cat_ieoce` for the CAT and IEOCE engines.
See :doc:`/cfl/classical/cert_cfl` for the CERT-CFL engine.

Dynamic Bidirected Dyck Reachability
------------------------------------

Exact single-language Dyck reachability under insertions and deletions of
complementary edge pairs. The POPL 2022 C++ algorithm retains degree-based
merging, path compression, linked worklists, and acyclic splitting. Cyclic
deletion safely recomputes the remaining graph.

**Location**: ``include/CFL/DynamicDyck/``, ``lib/CFL/DynamicDyck/``

See :doc:`/cfl/dynamic_dyck` for the reusable API, artifact formats, and
cycle-correction details.

Interleaved-Dyck Core
---------------------

Shared typed ``Label``, ``Edge``, ``Graph``, and ``Pair`` types plus the DOT
parser used by interleaved-Dyck benchmark datasets.

**Location**: ``include/CFL/InterleavedDyck/Core/``,
``lib/CFL/InterleavedDyck/Core/``

StagedBounds consumes this graph directly, Unary applies unary projection,
and MCFL converts it through a typed-to-generic adapter.

Exact Unary Interleaved Dyck
----------------------------

Exact component reachability for bidirected unary
``D1``-interleaved-``D1``. The module provides adaptive counter flattening and
the POPL 2022 fixed-counter exact baseline.

**Location**: ``include/CFL/InterleavedDyck/Unary/``,
``lib/CFL/InterleavedDyck/Unary/``

See :doc:`/cfl/interleaved_dyck/unary` for both algorithms, their exactness boundary,
and benchmark eligibility rules.

Interleaved-Dyck Staged Bounds
------------------------------

Staged under- and overapproximation for reachability under two interleaved
families of Dyck constraints.

**Location**: ``include/CFL/InterleavedDyck/StagedBounds/``,
``lib/CFL/InterleavedDyck/StagedBounds/``

This component computes a certified union-Dyck lower bound and progressively
tighter projected-CFL upper bounds; it is not an exact solver for the general
typed problem.

**Features**:

* DOT parsing for parenthesis, bracket, and neutral edges
* Dyck-over-the-union underapproximation
* Projected-language intersection and derivation-tracing mutual refinement
* Stronger parity grammar and pairwise on-demand refinement
* Taint and value-flow benchmark modes

Multiple Context-Free Language Reachability
-------------------------------------------

All-pairs reachability for non-deleting, non-permuting MCFGs and the POPL 2025
typed underapproximation hierarchy.

**Location**: ``include/CFL/InterleavedDyck/MCFL/``, ``lib/CFL/InterleavedDyck/MCFL/``

**Features**:

* All five normal-form MCFL rule types with structural validation
* Indexed worklist saturation and tuple reachability pruning
* Concrete path witnesses from retained derivation DAGs
* ``G_d^circ`` and ``G_d^+`` grammar generation for arbitrary dimensions
* Artifact-compatible staged condensation, DOT input, and command-line tool
* Adapter from the shared typed interleaved-Dyck graph

Guarantee Summary
-----------------

.. list-table:: Choosing an interleaved-Dyck implementation
   :header-rows: 1
   :widths: 31 39 30

   * - API
     - Intended use
     - Guarantee
   * - ``interleaved_dyck::mcfl::InterleavedDyckSolver``
     - Certified typed pairs through ``G_d``
     - Underapproximation
   * - ``interleaved_dyck::unary::FixedCounterSolver``
     - POPL 2022 exact fixed-counter baseline
     - Exact component partition
   * - ``interleaved_dyck::unary::AdaptiveSolver``
     - Bidirected unary projection
     - Exact component partition
   * - ``interleaved_dyck::staged_bounds::Solver``
     - Typed lower/upper refinement
     - Approximation bounds

CSIndex (Context-Sensitive Indexing)
------------------------------------

Context-sensitive indexing for CFL reachability.

**Location**: ``include/CFL/CSIndex/``, ``lib/CFL/CSIndex/``

The implementation is split into ``FLARE`` and ``SCS``. FLARE owns the
extended-Dyck graph and indexing algorithms; SCS builds policy products and
then reuses FLARE. See :doc:`/cfl/indexing/csindex` for the public namespaces and directory
layout.

**Features**: Context-aware indexing algorithms for efficient CFL queries.

**Components**:
* Context-sensitive graph indexing
* Reachability query optimization
* Memory-efficient representations


Interleaved-Dyck Graph Reduction
--------------------------------

PLDI 2020 interleaved-Dyck graph simplification. This component transforms a
DOT graph and does not itself return the final reachability relation.

**Location**: ``lib/CFL/InterleavedDyck/GraphReduction/``

**Features**:

* Two-color summary construction and degree-based node merging
* Iterative Python orchestration until no further edge is removed
* Explicit directed versus already-bidirected input mode
* Private legacy summary representation under the ``lib`` subtree

See also :doc:`/cfl/classical/classical`, :doc:`/cfl/indexing/csindex`,
:doc:`/cfl/interleaved_dyck/unary`,
:doc:`/cfl/interleaved_dyck/staged_bounds`,
:doc:`/cfl/interleaved_dyck/graph_reduction`,
:doc:`/cfl/interleaved_dyck/mcfl`.
