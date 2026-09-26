CFL Tools
=========

This page documents the CFL-related tools under ``tools/cfl/``. For the
underlying theory and components, see :doc:`/cfl/cfl_components`.

Overview
--------

Context-Free Language (CFL) reachability extends graph reachability with
context-free grammars for precise interprocedural analysis. CFL reachability
enables analysis of complex program properties using grammar-based constraints.

**Location**: ``tools/cfl/``

**Tools**: ``lotus-cfl-solve``, ``lotus-cfl-alias``, ``lotus-cfl-vf``,
``lotus-cfl-interleaved-dyck-mcfl``, ``lotus-cfl-interleaved-dyck-staged-bounds``,
``lotus-cfl-interleaved-dyck-unary``, ``lotus-cfl-interleaved-dyck-spds``,
``lotus-cfl-interleaved-dyck-lcl``, ``lotus-cfl-interleaved-dyck-affine-spds``,
``lotus-cfl-interleaved-dyck-graph-reduction``, ``lotus-cfl-dynamic-dyck``, and CSR.

Classical CFL solving and clients
---------------------------------

``lotus-cfl-solve`` runs a supplied grammar over a text, DOT, or JSON
graph. Select the engine with ``--solver``; the available backends include
``sparse-set``, ``sparse-bitvector``, ``graspan``, ``transitive-closure``,
``pocr``, ``hpocr``, ``focr``, ``pearl``, ``stg``, ``sqid``, ``skewed``,
``endpoint-quotient``, ``cert``, ``cat``, ``iea``, and ``iea-ocr``.

``lotus-cfl-alias`` consumes LLVM IR or bitcode. It uses Aser as the constraint
frontend but drives points-to propagation and indirect-call discovery through
the CFL relation. It supports PAG/PEG encodings, annotation validation,
points-to printing, named alias queries, and JSON statistics.

``lotus-cfl-vf`` is the value-flow counterpart. It builds a sparse value-flow
graph from LLVM IR, applies the CFL-specific strong-update preparation, and
answers context-sensitive pointer value-flow queries with matched call/return
labels.

.. code-block:: bash

   cmake --build build --target lotus-cfl-solve lotus-cfl-alias lotus-cfl-vf
   build/bin/lotus-cfl-solve --grammar grammar.txt --graph graph.txt \
     --solver transitive-closure --json-stats
   build/bin/lotus-cfl-alias --encoding cfl-peg --solver sparse-bitvector \
     --check-annotations module.bc
   build/bin/lotus-cfl-vf --solver transitive-closure \
     --query main::source,main::sink module.bc

Use ``--solver pocr``, ``--solver hpocr``, or ``--solver focr`` to select the
ported POCR algorithm families. The alias driver defaults every backend to the
same ``cfl-peg`` client pipeline: the standard alias grammar, SCC elimination,
and PEG folding. ``--encoding pag`` and ``--encoding peg`` explicitly select
Lotus's distinct PAG grammar or extended PEG grammar (with ``ArrayPath`` and
``Memcpy`` summaries). These modes do not compute identical grammar relations;
``cfl-peg`` is not merely a compressed spelling of ``peg``. The same solver
selectors are available to the value-flow client.

Use ``--solver skewed`` to select PLDI 2024 skewed tabulation through the same
complete-relation client interface.

Use ``--solver stg --stg-spec FILE`` for ISSTA 2024 staged solving. STG needs
an explicit JSON decomposition because an arbitrary CFG does not uniquely
identify its context-free pattern, delimiters, or regular phases. See
:doc:`/cfl/classical/stg` for the schema.

Use ``--solver cat`` for the ICSE 2026 context-aware tabulation engine, and
``--solver iea`` or ``--solver iea-ocr`` for the OOPSLA 2024 iterative-epoch
online cycle elimination variants (``iea-ocr`` additionally applies online
cycle reduction and minimum-equivalent graphs). ``--solver endpoint-quotient``
selects the grammar-indexed endpoint-quotient (GEQ) compressed exact engine.
See :doc:`/cfl/classical/cat_ieoce` for the algorithm and API details.

Use ``--solver cert`` to select the cardinality-certified exact all-symbol CFL
reachability engine. See :doc:`/cfl/classical/cert_cfl` for architecture and
options.

The general solver accepts the vendored datasets under
``benchmarks/real-world/CFL/Classical``, POCR grammar/graph files, and exposes
unidirectional summarization, SCC elimination, graph folding, and inter-Dyck
pruning. Foldability utilities remain available through their C++ APIs.
Run ``python3 scripts/cfl/run_cfl_dataset.py --all-solvers --case lbm`` for a
small cross-backend check over the vendored alias, value-flow, and taint
instances. This includes ``cert``; STG remains separate because it requires an
explicit decomposition specification.
Select subsets with comma-separated or repeated ``--analysis``, ``--case``,
and ``--solver`` options. ``--workers N`` runs independent solver processes in
parallel and ``--timeout SECONDS`` applies a limit to each process. The driver
emits success, failure, and timeout records as JSONL. ``Ctrl-C`` terminates all
active solver process groups before the driver exits.
``--memory-limit-mb MB`` additionally applies a sampled per-process RSS cap on
Linux and macOS; account for the worker count when choosing it.
Use ``--resume --output FILE`` to continue only task keys missing from an
interrupted JSONL run.

See :doc:`/cfl/classical/classical`, :doc:`/cfl/classical/pearl`,
:doc:`/cfl/classical/stg`, and :doc:`/cfl/classical/sqid` for the complete algorithm,
option, and API descriptions.

Dynamic Bidirected Dyck Reachability
------------------------------------

``lotus-cfl-dynamic-dyck`` consumes the original string-ID DOT records with
``op--TYPE``/``cp--TYPE`` labels and an ``A``/``D`` edge update sequence. Each update
operates on a complementary bidirected edge pair. It maintains exact
single-language Dyck components, including cycle-safe deletion.

.. code-block:: bash

   cmake --build build --target lotus-cfl-dynamic-dyck
   build/bin/lotus-cfl-dynamic-dyck --print-components initial.dot updates.seq
   build/bin/lotus-cfl-dynamic-dyck --recompute initial.dot updates.seq

The original artifact mode selectors ``0`` (recompute) and ``1`` (dynamic)
are accepted before the two input paths. Default output preserves the original
elapsed-seconds value and trailing space. Run the table scripts directly as
``bash scripts/cfl/dynamic-dyck/gen_table3.sh`` or ``gen_table4.sh``; they resolve
repository paths and accept a ``DYCK_REACH_BINARY`` override for other builds.
See :doc:`/cfl/dynamic_dyck` for
formats, edge-pair semantics, library usage, and deletion limitations.

MCFL: Multiple Context-Free Language Reachability
-------------------------------------------------

Runs the POPL 2025 MCFL hierarchy for underapproximating interleaved-Dyck
reachability on artifact-compatible DOT graphs.

**Binary**: ``lotus-cfl-interleaved-dyck-mcfl``

**Location**: ``tools/cfl/interleaved-dyck/mcfl/lotus-cfl-interleaved-dyck-mcfl.cpp``

.. code-block:: bash

   cmake -S . -B build -DLOTUS_ENABLE_CFL=ON
   cmake --build build --target lotus-cfl-interleaved-dyck-mcfl
   ./build/bin/lotus-cfl-interleaved-dyck-mcfl --dimension 2 input.dot

Useful options include ``--simple`` for the weaker ``G_d^circ`` grammar,
``--no-condense`` to disable cycle elimination, ``--stats`` for saturation
counters, ``--artifact-compatible`` for exact condensed cross-product
expansion, ``--print-pairs`` for the final relation, and ``-o FILE`` for file
output. See :doc:`/cfl/interleaved_dyck/mcfl` for the library API and
algorithm details.

CSR: Context-Sensitive Reachability
-----------------------------------

Indexing-based context-sensitive reachability engine for large graphs.

**Binary**: ``csr``  
**Location**: ``tools/cfl/csr/csr.cpp``

CSR operates on graph files (not LLVM bitcode directly) and answers reachability
queries with different indexing strategies (GRAIL, PathTree, or combined).
The driver is implemented against the reorganized ``CFL/CSIndex/FLARE`` API;
sanitizer-aware policy products live separately under ``CFL/CSIndex/SCS``.

**Basic Usage**:

.. code-block:: bash

   ./build/bin/csr [options] graph_file

**Common Options** (see ``tools/cfl/csr/README.md`` for full list):

- ``-m <method>`` – Indexing method:

  - ``pathtree`` – PathTree indexing
  - ``grail`` – GRAIL labeling
  - ``pathtree+grail`` – Combined approach

- ``-t`` – Evaluate transitive closure
- ``-r`` – Evaluate tabulation algorithm
- ``-p`` – Evaluate parallel tabulation algorithm
- ``-j <N>`` – Number of threads for parallel tabulation (0 = auto)
- ``-n <N>`` – Number of reachable/unreachable queries to generate (default 100 each)
- ``-d <N>`` – Dimension for GRAIL labeling (default 2)
- ``-g <file>`` – Generate queries and save to file
- ``-q <file>`` – Load queries from file

**Examples**:

.. code-block:: bash

   # GRAIL-based reachability
   ./build/bin/csr input.graph

   # PathTree indexing
   ./build/bin/csr -m pathtree input.graph

   # Parallel tabulation with 4 threads
   ./build/bin/csr -p -j 4 input.graph

Interleaved-Dyck Staged Bounds
------------------------------

Computes staged lower and upper bounds for typed interleaved-Dyck reachability
on a DOT graph: a certified lower bound, then progressively tighter
overapproximations through parity refinement, mutual refinement, and on-demand
checks.

**Binary**: ``lotus-cfl-interleaved-dyck-staged-bounds``

**Location**: ``tools/cfl/interleaved-dyck/staged-bounds/lotus-cfl-interleaved-dyck-staged-bounds.cpp``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-staged-bounds
   build/bin/lotus-cfl-interleaved-dyck-staged-bounds \
     --method mutual-refinement graph.dot

Useful options include ``--analysis taint|value-flow`` to select the client
analysis (default ``taint``), ``--method`` to select one algorithm or the
full pipeline, ``--print-lower``/``--print-result`` for pair output, and
``-o FILE`` for file output. See
:doc:`/cfl/interleaved_dyck/staged_bounds` for the library API and
algorithm details.

Unary Interleaved-Dyck Reachability
-----------------------------------

Computes exact bidirected unary ``D1``-interleaved-``D1`` reachability on a DOT
graph with the adaptive (default) or fixed-counter algorithm.

**Binary**: ``lotus-cfl-interleaved-dyck-unary``

**Location**: ``tools/cfl/interleaved-dyck/unary/lotus-cfl-interleaved-dyck-unary.cpp``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-unary
   build/bin/lotus-cfl-interleaved-dyck-unary --algorithm adaptive graph.dot

Useful options include ``--direct`` to skip quotient sparsification,
``--bidirect`` to add missing complement reverse arcs (a sound
overapproximation of the original directed graph), ``--shallow K`` for the
adaptive-only shallow solve, ``--stats`` for construction and backend
statistics, and ``--print-pairs`` to materialize non-reflexive component
pairs. See :doc:`/cfl/interleaved_dyck/unary` for the library API and
algorithm details.

LCL Reachability
----------------

Computes the POPL 2017 LCL upper bound for directed, typed interleaved-Dyck
reachability on a DOT graph. A retained pair is may-reach rather than a
certified balanced witness; absence proves unreachability in the supplied
directed graph.

**Binary**: ``lotus-cfl-interleaved-dyck-lcl``

**Location**: ``tools/cfl/interleaved-dyck/lcl/lotus-cfl-interleaved-dyck-lcl.cpp``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-lcl
   build/bin/lotus-cfl-interleaved-dyck-lcl --query 0 3 graph.dot

Useful options include ``--baseline`` for the Algorithm 1 white-node baseline,
``--no-feasibility`` to disable the Section 5.3 endpoint filters,
``--print-upper`` to print sorted upper-bound pairs, and
``--max-summaries``/``--max-normalized-edges`` for hard limits (exceeding one
throws rather than returning a partial upper bound). See
:doc:`/cfl/interleaved_dyck/lcl` for the library API and algorithm
details.

SPDS Reachability
-----------------

Solves synchronized pushdown systems for balanced interleaved-Dyck
reachability, supporting post*/pre* demand queries over a DOT graph.

**Binary**: ``lotus-cfl-interleaved-dyck-spds``

**Location**: ``tools/cfl/interleaved-dyck/spds/lotus-cfl-interleaved-dyck-spds.cpp``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-spds
   build/bin/lotus-cfl-interleaved-dyck-spds --all-pairs graph.dot

Vertices are integer IDs. Query scopes include ``--all-pairs``,
``--query SOURCE TARGET``, ``--source V`` (post*), ``--target V`` (pre*), and
``--queries FILE``.
``--call-prefix``/``--field-prefix`` allow pending calls or stores at a forward
endpoint, and ``--max-states``/``--max-transitions``/``--max-updates`` bound
each projection. Results are a sound upper bound; resource exhaustion exits
with code 3 and yields no result. See
:doc:`/cfl/interleaved_dyck/spds` for the library API and algorithm
details.

Affine SPDS Reachability
------------------------

Combines paired affine-weighted pushdown automata with SPDS reachability and
reports affine separation certificates.

**Binary**: ``lotus-cfl-interleaved-dyck-affine-spds``

**Location**: ``tools/cfl/interleaved-dyck/affine-spds/lotus-cfl-interleaved-dyck-affine-spds.cpp``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-affine-spds
   build/bin/lotus-cfl-interleaved-dyck-affine-spds --query 0 3 graph.dot

Select the comparison with ``--mode joint|independent|spds`` (``--identity``
gives the Boolean SPDS specialization), request a separating affine equation
with ``--certificate``, and emit machine-readable output with ``--json``.
Exact endpoint histories can be pinned with ``--call-stack``/``--field-stack``,
and a shared observer map can be loaded or dumped with
``--observer``/``--dump-observer``. See
:doc:`/cfl/interleaved_dyck/affine_spds` for the library API and algorithm
details.

Interleaved-Dyck Graph Reduction
--------------------------------

Implements the PLDI 2020 graph-simplification pipeline for interleaved-Dyck
reachability. It is a graph transformation, not a reachability solver: it
edits a working copy of a DOT graph in place and produces a smaller graph that
preserves the reachability property covered by the reduction theorem.

**Binary**: ``lotus-cfl-interleaved-dyck-graph-reduction`` (Python driver) with the
compiled helpers ``lotus-cfl-interleaved-dyck-graphaux`` and
``lotus-cfl-interleaved-dyck-dkmerge``

**Location**: ``tools/cfl/interleaved-dyck/graph-reduction/``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-graph-reduction
   cp input.dot reduced.dot
   python3 build/bin/lotus-cfl-interleaved-dyck-graph-reduction.py reduced.dot \
     --graphaux build/bin/lotus-cfl-interleaved-dyck-graphaux \
     --dkmerge build/bin/lotus-cfl-interleaved-dyck-dkmerge

``lotus-cfl-interleaved-dyck-graphaux`` performs one-color component construction
(``lotus-cfl-interleaved-dyck-graphaux <graph.dot>``) and
``lotus-cfl-interleaved-dyck-dkmerge`` performs the degree-based merge phase; the
Python driver alternates both colors and removes proven-redundant edges. Pass
``--bidirected-input`` when the input already represents both directions. See
:doc:`/cfl/interleaved_dyck/graph_reduction` for the library API and algorithm
details.
