Alias Analysis Components
==========================

Lotus provides several alias analysis algorithms with different precision/performance trade-offs.

AllocAA
-------

Simple heuristic-based alias analysis for basic allocation tracking.

**Location**: ``lib/Alias/AllocAA``

**Usage**: Integrated as LLVM ModulePass for basic alias queries.

AserAA
------

High-performance pointer analysis with multiple context sensitivities and solver algorithms.

**Location**: ``lib/Alias/AserAA``

**Analysis Modes**:
* ``ci``: Context-insensitive (default)
* ``1-cfa``: 1-call-site sensitive
* ``2-cfa``: 2-call-site sensitive
* ``origin``: Origin-sensitive (tracks thread creation)

**Solver Types**:
* ``basic``: PartialUpdateSolver
* ``wave``: WavePropagation with SCC detection (default)
* ``deep``: DeepPropagation with cycle detection

**Usage**:
.. code-block:: bash

   ./build/bin/aser-aa -analysis-mode=1-cfa -solver=deep example.bc

**Key Options**:
* ``-analysis-mode=<mode>``: Analysis mode (ci, 1-cfa, 2-cfa, origin)
* ``-solver=<type>``: Solver algorithm (basic, wave, deep)
* ``-field-sensitive``: Use field-sensitive memory model (default: true)
* ``-dump-stats``: Print analysis statistics
* ``-consgraph``: Dump constraint graph to DOT file

DyckAA
------

Unification-based, exhaustive alias analysis with high precision.

**Location**: ``lib/Alias/DyckAA``

**Usage**:
.. code-block:: bash

   ./build/bin/dyck-aa -print-alias-set-info example.bc

**Key Options**:
* ``-print-alias-set-info``: Print alias sets in DOT format
* ``-count-fp``: Count function pointers
* ``-dot-dyck-callgraph``: Generate call graph visualization
* ``-no-function-type-check``: Disable function type checking

CFLAA
-----

Context-Free Language Alias Analysis from LLVM 14.0.6.

**Location**: ``lib/Alias/CFLAA``

**Note**: Copied from LLVM 14.0.6 (removed in LLVM 15).

Sea-DSA
-------

Context-sensitive, field-sensitive alias analysis based on DSA.

**Location**: ``lib/Alias/seadsa``

**Tools**: ``sea-dsa-dg``, ``seadsa-tool``

**Usage**:
.. code-block:: bash

   ./build/bin/sea-dsa-dg --sea-dsa-dot example.bc
   ./build/bin/seadsa-tool --sea-dsa-dot --outdir results/ example.bc

**Key Options**:
* ``--sea-dsa-dot``: Generate DOT files visualizing memory graphs
* ``--sea-dsa-callgraph-dot``: Generate call graph DOT files
* ``--outdir <DIR>``: Specify output directory

Andersen
--------

Context-insensitive points-to analysis for scalability.

**Location**: ``lib/Alias/Andersen``

**Usage**:
.. code-block:: bash

   ./build/bin/ander-aa example.bc

**Features**:
* Andersen's inclusion-based algorithm
* No on-the-fly callgraph construction
* Fast analysis for large codebases

FPA (Function Pointer Analysis)
-------------------------------

Multiple algorithms for resolving indirect function calls.

**Location**: ``lib/Alias/FPA``

**Algorithms**:
* ``FLTA`` (1): Flow-insensitive, type-based analysis
* ``MLTA`` (2): Multi-layer type analysis
* ``MLTADF`` (3): Multi-layer type analysis with data flow
* ``KELP`` (4): Context-sensitive analysis (USENIX Security'24)

**Usage**:
.. code-block:: bash

   ./build/bin/fpa -analysis-type=1 example.bc  # FLTA
   ./build/bin/fpa -analysis-type=2 -max-type-layer=10 example.bc  # MLTA

**Key Options**:
* ``-analysis-type=<N>``: Select algorithm (1-4)
* ``-max-type-layer=<N>``: Maximum type layer for MLTA (default: 10)
* ``-debug``: Enable debug output
* ``-output-file=<path>``: Output file path

OriginAA
--------

K-callsite sensitive and origin-sensitive pointer analysis.

**Location**: ``lib/Alias/OriginAA``

**Usage**:
.. code-block:: bash

   ./build/bin/origin_aa -analysis-mode=kcs -k=2 example.bc

**Features**:
* K-callsite sensitivity for precision
* Origin sensitivity for thread creation tracking
* Tracks pthread_create and spawn operations

DynAA
-----

Dynamic validation of static alias analysis results.

**Location**: ``tools/dynaa``

**Workflow**:
1. Instrument: ``./build/bin/dynaa-instrument example.bc -o example.inst.bc``
2. Compile: ``clang example.inst.bc libRuntime.a -o example.inst``
3. Run: ``LOG_DIR=logs/ ./example.inst``
4. Check: ``./build/bin/dynaa-check example.bc logs/pts.log basic-aa``

**Tools**:
* ``dynaa-instrument``: Instrument code for runtime tracking
* ``dynaa-check``: Validate static analysis against runtime
* ``dynaa-log-dump``: Convert binary logs to readable format


