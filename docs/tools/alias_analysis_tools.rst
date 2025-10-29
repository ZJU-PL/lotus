Alias Analysis Tools
=====================

Command-line tools for alias analysis in Lotus.

DyckAA Tool (dyck-aa)
--------------------

**Binary**: ``build/bin/dyck-aa``

Unification-based alias analysis tool.

**Usage**:
.. code-block:: bash

   ./build/bin/dyck-aa [options] <input bitcode file>

**Key Options**:
* ``-print-alias-set-info``: Print alias sets in DOT format
* ``-count-fp``: Count function pointer targets
* ``-dot-dyck-callgraph``: Generate call graph visualization
* ``-no-function-type-check``: Disable function type checking

**Examples**:
.. code-block:: bash

   ./build/bin/dyck-aa -print-alias-set-info example.bc
   ./build/bin/dyck-aa -dot-dyck-callgraph -with-labels example.bc

AserPTA Tool
------------

**Binary**: ``build/bin/aser-aa``

High-performance pointer analysis with multiple context sensitivities and solver algorithms.

**Usage**:
.. code-block:: bash

   ./build/bin/aser-aa [options] <input bitcode file>

**Key Options**:
* ``-analysis-mode=<mode>``: ``ci`` (context-insensitive), ``1-cfa``, ``2-cfa``, ``origin``
* ``-solver=<type>``: ``basic``, ``wave`` (default), ``deep``
* ``-field-sensitive``: Use field-sensitive memory model (default: true)
* ``-dump-stats``: Print analysis statistics
* ``-consgraph``: Dump constraint graph to DOT file
* ``-dump-pts``: Dump points-to sets

**Examples**:
.. code-block:: bash

   # Context-insensitive with wave propagation
   ./build/bin/aser-aa example.bc
   
   # 1-CFA with deep propagation solver
   ./build/bin/aser-aa -analysis-mode=1-cfa -solver=deep example.bc
   
   # Origin-sensitive (tracks thread creation)
   ./build/bin/aser-aa -analysis-mode=origin example.bc

FPA Tool
--------

**Binary**: ``build/bin/fpa``

Function Pointer Analysis with multiple algorithms.

**Usage**:
.. code-block:: bash

   ./build/bin/fpa [options] <input bitcode files>

**Key Options**:
* ``-analysis-type=<N>``: ``1``=FLTA, ``2``=MLTA, ``3``=MLTADF, ``4``=KELP
* ``-max-type-layer=<N>``: Set maximum type layer for MLTA
* ``-output-file=<path>``: Output file path

**Examples**:
.. code-block:: bash

   ./build/bin/fpa -analysis-type=1 example.bc  # FLTA
   ./build/bin/fpa -analysis-type=2 -output-file=results.txt example.bc

DynAA Tools
-----------

**Binaries**: ``build/bin/dynaa-instrument``, ``build/bin/dynaa-check``, ``build/bin/dynaa-log-dump``

Dynamic validation of static analysis results.

**Workflow**:
.. code-block:: bash

   # 1. Instrument
   ./build/bin/dynaa-instrument example.bc -o example.inst.bc
   
   # 2. Compile and run
   clang example.inst.bc libRuntime.a -o example.inst
   LOG_DIR=logs/ ./example.inst
   
   # 3. Check results
   ./build/bin/dynaa-check example.bc logs/pts.log basic-aa

Sea-DSA Tools
-------------

**Binaries**: ``build/bin/sea-dsa-dg``, ``build/bin/seadsa-tool``

Memory graph generation and analysis.

**Usage**:
.. code-block:: bash

   ./build/bin/sea-dsa-dg --sea-dsa-dot example.bc
   ./build/bin/seadsa-tool --sea-dsa-dot --outdir results/ example.bc

Tool Comparison
---------------

| Tool | Precision | Performance | Best For |
|------|-----------|-------------|----------|
| dyck-aa | Very High | Very High | Exhaustive analysis |
| aser-aa | High | Moderate | Context-sensitive analysis |
| fpa     | High | Good | Function pointers |
| dynaa-* | Runtime | Runtime | Validation |
| sea-dsa-* | High | Good | Context-sensitive |
