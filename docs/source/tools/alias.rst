Alias Analysis Tools
====================

This page documents the command-line tools under ``tools/alias/``. For the
underlying algorithms and architecture, see :doc:`../alias/alias_analysis`.

Andersen (ander-aa)
-------------------

Context-insensitive inclusion-based points-to analysis.

**Binary**: ``ander-aa``  
**Location**: ``tools/alias/ander-aa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/ander-aa [options] input.bc

**Notes**:

- Fast, scalable analysis for large codebases
- No on-the-fly call graph construction
- Good default when you need quick alias information

AserPTA (aser-aa)
-----------------

High-performance constraint-based pointer analysis with multiple context
sensitivities and solver algorithms.

**Binary**: ``aser-aa``  
**Location**: ``tools/alias/aser-aa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/aser-aa [options] input.bc

**Key Options** (see also :doc:`../alias/alias_analysis` and ``TOOLS.md``):

- **Analysis mode**:

  - ``-analysis-mode=ci`` – Context-insensitive (default)
  - ``-analysis-mode=1-cfa`` – 1-call-site sensitive
  - ``-analysis-mode=2-cfa`` – 2-call-site sensitive
  - ``-analysis-mode=origin`` – Origin-sensitive (thread creation)

- **Solver**:

  - ``-solver=basic`` – PartialUpdateSolver
  - ``-solver=wave`` – WavePropagation with SCC detection (default)
  - ``-solver=deep`` – DeepPropagation with cycle-aware propagation

- **Other**:

  - ``-field-sensitive[=true|false]`` – Field-sensitive memory model
  - ``-dump-stats`` – Print analysis statistics
  - ``-consgraph`` – Dump constraint graph (DOT)
  - ``-dump-pts`` – Dump points-to sets

**Example**:

.. code-block:: bash

   # 1-CFA with deep solver
   ./build/bin/aser-aa -analysis-mode=1-cfa -solver=deep input.bc

DyckAA (dyck-aa)
----------------

Unification-based alias analysis using Dyck-CFL reachability.

**Binary**: ``dyck-aa``  
**Location**: ``tools/alias/dyck-aa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/dyck-aa [options] input.bc

**Key Options**:

- ``-print-alias-set-info`` – Print alias sets and relations (DOT)
- ``-count-fp`` – Count possible targets for each function pointer
- ``-dot-dyck-callgraph`` – Generate call graph from alias results
- ``-no-function-type-check`` – Disable function type compatibility checking

**Typical Use**:

.. code-block:: bash

   # Dump alias sets and Dyck-based call graph
   ./build/bin/dyck-aa -print-alias-set-info -dot-dyck-callgraph input.bc

LotusAA (lotus-aa)
------------------

Lotus-specific, flow-sensitive and field-sensitive pointer analysis with
on-the-fly call graph construction.

**Binary**: ``lotus-aa``  
**Location**: ``tools/alias/lotus-aa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/lotus-aa [options] input.bc

**Characteristics**:

- Flow-sensitive intra-procedural analysis
- Inter-procedural summarization and iterative call graph refinement
- Designed for high precision on smaller/medium programs

FPA (Function Pointer Analysis)
-------------------------------

Function pointer analysis toolbox (FLTA, MLTA, MLTADF, KELP) for resolving
indirect calls.

**Binary**: ``fpa``  
**Location**: ``tools/alias/fpa.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/fpa [options] input.bc

**Key Options**:

- ``-analysis-type=<N>``:

  - ``1`` – FLTA
  - ``2`` – MLTA
  - ``3`` – MLTADF
  - ``4`` – KELP (USENIX Security'24)

- ``-max-type-layer=<N>`` – Maximum type layer for MLTA (default: 10)
- ``-debug`` – Enable debug output
- ``-output-file=<path>`` – Output file (\"cout\" for stdout)

**Example**:

.. code-block:: bash

   # KELP-based function pointer analysis
   ./build/bin/fpa -analysis-type=4 -debug input.bc

Sea-DSA Tools
-------------

Sea-DSA-based memory graph construction and analysis.

**Binaries**:

- ``sea-dsa-dg`` – Simple Sea-DSA driver
- ``seadsa-tool`` – Advanced Sea-DSA analysis tool

**Locations**:

- ``tools/alias/sea-dsa-dg.cpp``
- ``tools/alias/seadsa-tool.cpp``

**Usage**:

.. code-block:: bash

   # Basic memory graph generation
   ./build/bin/sea-dsa-dg --sea-dsa-dot input.bc

   # Advanced analysis with DOT output directory
   ./build/bin/seadsa-tool --sea-dsa-dot --outdir=results/ input.bc

**Key Options**:

- ``--sea-dsa-dot`` – Generate DOT memory graphs
- ``--sea-dsa-callgraph-dot`` – Generate call graph DOT (if enabled)
- ``--outdir <DIR>`` – Output directory for generated artifacts

DynAA (Dynamic Alias Analysis)
------------------------------

Dynamic checker that validates static alias analyses against runtime behavior.

**Binaries**:

- ``dynaa-instrument`` – Instrument program to log pointer behavior
- ``dynaa-check`` – Compare runtime logs against static AA
- ``dynaa-log-dump`` – Dump binary logs to readable format

**Location**: ``tools/alias/dynaa/``

**Typical Workflow**:

.. code-block:: bash

   # 1. Compile to LLVM bitcode
   clang -emit-llvm -c example.cpp -o example.bc

   # 2. Instrument the bitcode
   ./build/bin/dynaa-instrument example.bc -o example.inst.bc

   # 3. Build and run the instrumented binary
   clang example.inst.bc build/libRuntime.a -o example.inst
   LOG_DIR=logs/ ./example.inst

   # 4. Check static analysis (e.g., basic-aa) against runtime log
   ./build/bin/dynaa-check example.bc logs/pts.log basic-aa

   # 5. Optionally dump the log
   ./build/bin/dynaa-log-dump logs/pts.log

**Use Cases**:

- Validate soundness/precision of a static alias analysis
- Investigate mismatches between static and dynamic behavior
- Support research on alias analysis accuracy

See also the detailed description in ``tools/alias/dynaa/README.md``.
