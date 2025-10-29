Alias Analysis Components
==========================

Lotus provides several alias analysis algorithms with different precision/performance trade-offs.

DyckAA
------

Unification-based, exhaustive alias analysis with high precision.

**Location**: ``lib/Alias/DyckAA``

**Usage**:
.. code-block:: bash

   ./build/bin/dyck-aa -print-alias-set-info example.bc

**Key Options**:
* ``-print-alias-set-info``: Print alias sets in DOT format
* ``-dot-dyck-callgraph``: Generate call graph visualization

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

Andersen
--------

Context-insensitive points-to analysis for scalability.

**Location**: ``lib/Alias/Andersen``

FPA (Function Pointer Analysis)
-------------------------------

Multiple algorithms for resolving indirect function calls.

**Location**: ``lib/Alias/FPA``

**Algorithms**: FLTA, MLTA, MLTADF, KELP

**Usage**:
.. code-block:: bash

   ./build/bin/fpa -analysis-type=1 example.bc  # FLTA

OriginAA
--------

K-callsite sensitive and origin-sensitive pointer analysis.

**Location**: ``lib/Alias/OriginAA``

**Usage**:
.. code-block:: bash

   ./build/bin/origin_aa -analysis-mode=kcs -k=2 example.bc

DynAA
-----

Dynamic validation of static alias analysis results.

**Location**: ``tools/dynaa``

**Workflow**:
1. Instrument: ``./build/bin/dynaa-instrument example.bc -o example.inst.bc``
2. Run: ``clang example.inst.bc libRuntime.a -o example.inst && LOG_DIR=logs/ ./example.inst``
3. Check: ``./build/bin/dynaa-check example.bc logs/pts.log basic-aa``

Tool Selection
--------------

| Tool | Precision | Performance | Best For |
|------|-----------|-------------|----------|
| DyckAA | High | Moderate | General analysis |
| OriginAA | Very High | Moderate | Precise analysis |
| Andersen | Low | High | Large programs |
| Sea-DSA | High | Good | Memory analysis |
| FPA | Variable | Variable | Function pointers |
| DynAA | Runtime | Runtime | Validation |
