Alias Analysis Components
==========================

Lotus provides several alias analysis algorithms with different precision/performance trade-offs. Each analysis makes different trade-offs between precision, scalability, and analysis cost.

Analysis Selection Guide
-------------------------

Choose the right analysis for your needs:

**For Large Codebases (Speed Priority)**:

- **Andersen**: Fastest, context-insensitive, inclusion-based
- **AserPTA (CI mode)**: Fast with field sensitivity option
- **AllocAA**: Lightweight heuristic-based tracking

**For Balanced Precision/Performance**:

- **AserPTA (1-CFA)**: Good precision with reasonable cost
- **FPA (KELP)**: Specialized for function pointer resolution
- **SRAA**: Range-based for proving non-aliasing

**For Maximum Precision**:

- **DyckAA**: Unification-based with Dyck-CFL reachability
- **LotusAA**: Flow-sensitive and field-sensitive
- **Sea-DSA**: Context-sensitive memory graphs

**For Specific Scenarios**:

- **Concurrent programs**: AserPTA with origin sensitivity
- **Dynamic validation**: DynAA for runtime verification
- **Function pointers only**: FPA (FLTA, MLTA, KELP)

Comparison Table
----------------

.. list-table::
   :header-rows: 1
   :widths: 20 15 15 15 15 20

   * - Analysis
     - Context
     - Flow
     - Field
     - Speed
     - Use Case
   * - Andersen
     - Insensitive
     - Insensitive
     - Insensitive
     - Fast
     - Quick scanning
   * - AserPTA-CI
     - Insensitive
     - Insensitive
     - Sensitive
     - Fast
     - Balanced analysis
   * - AserPTA-1CFA
     - 1-Call-Site
     - Insensitive
     - Sensitive
     - Moderate
     - Good precision
   * - DyckAA
     - Insensitive
     - Insensitive
     - Sensitive
     - Slow
     - High precision
   * - LotusAA
     - Sensitive
     - Sensitive
     - Sensitive
     - Slowest
     - Maximum precision
   * - Sea-DSA
     - Sensitive
     - Insensitive
     - Sensitive
     - Moderate
     - Memory graphs
   * - FPA-KELP
     - Sensitive
     - Insensitive
     - Sensitive
     - Moderate
     - Function pointers

AllocAA
-------

Simple heuristic-based alias analysis for basic allocation tracking.

**Location**: ``lib/Alias/AllocAA``

**Algorithm**: Tracks allocation sites and uses simple heuristics to determine aliasing.

**Strengths**:

- Extremely fast
- Low memory overhead
- Good for preliminary analysis

**Limitations**:

- Imprecise, many false positives
- No interprocedural analysis
- Limited to allocation sites

**Usage**: Integrated as LLVM ModulePass for basic alias queries.

AserPTA
-------

High-performance pointer analysis with multiple context sensitivities and solver algorithms.

**Location**: ``lib/Alias/AserPTA``

**Architecture**:

.. code-block:: text

   LLVM IR
      ↓
   [Preprocessing]
      ├─ Canonicalize GEP instructions
      ├─ Lower memcpy operations
      ├─ Normalize heap APIs
      └─ Remove exception handlers
      ↓
   [Constraint Collection]
      ├─ addr_of: p = &obj
      ├─ copy: p = q
      ├─ load: p = *q
      ├─ store: *p = q
      └─ offset: p = &obj->field
      ↓
   [Constraint Graph Construction]
      ├─ Pointer nodes (CGPtrNode)
      ├─ Object nodes (CGObjNode)
      └─ Constraint edges
      ↓
   [Solving with Context]
      ├─ Context evolution (NoCtx/KCallSite/KOrigin)
      ├─ Propagation strategy
      └─ SCC detection & collapsing
      ↓
   Points-to Sets (BitVector representation)

**Constraint Types**:

1. **Address-of** (``p = &obj``): Pointer directly addresses object
2. **Copy** (``p = q``): Pointer assignment
3. **Load** (``p = *q``): Dereference and copy
4. **Store** (``*p = q``): Store through pointer
5. **Offset** (``p = &obj->field``): Field access via GEP

**Memory Models**:

- **Field-Insensitive**: Treats objects as single entities (faster)
- **Field-Sensitive**: Models individual fields (more precise)

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

Unification-based, exhaustive alias analysis with high precision using Dyck-CFL reachability.

**Location**: ``lib/Alias/DyckAA``

**Algorithm**: Uses Dyck Context-Free Language (CFL) reachability to model pointer relationships:

.. code-block:: text

   LLVM IR
      ↓
   [Build Dyck Graph]
      ├─ Nodes: Values (pointers, objects)
      ├─ Edges with labels:
      │  ├─ *(* : Dereference
      │  ├─ *)* : Reference
      │  ├─ *[field]* : Field access
      │  └─ Assignment edges
      ↓
   [Dyck-CFL Reachability]
      ├─ Find balanced paths
      ├─ Unify equivalent nodes
      └─ Build alias sets
      ↓
   [Applications]
      ├─ Alias queries
      ├─ Call graph construction
      ├─ ModRef analysis
      └─ Value flow analysis

**Dyck Language**: Balanced parentheses language that captures pointer semantics:

- ``( ... )`` : Dereference operations must balance
- ``[ ... ]`` : Field accesses must match
- Paths between nodes indicate aliasing

**Example**:

.. code-block:: c

   int x;
   int *p = &x;    // Edge: p -*)->* x
   int **q = &p;   // Edge: q -*)->* p
   int *r = *q;    // Path: r = *q, q points to p, so r = p
                   // Dyck path: r -*(*-* q -*)->* p

**Strengths**:

- Highly precise through CFL reachability
- Handles complex pointer patterns
- Good for function pointer resolution
- Builds precise call graphs

**Limitations**:

- Computationally expensive
- High memory usage for large programs
- Context-insensitive (single analysis per function)

**Usage**:
.. code-block:: bash

   ./build/bin/dyck-aa -print-alias-set-info example.bc

**Key Options**:
* ``-print-alias-set-info``: Print alias sets in DOT format
* ``-count-fp``: Count function pointers
* ``-dot-dyck-callgraph``: Generate call graph visualization
* ``-no-function-type-check``: Disable function type checking

**Advanced Features**:

- **DyckVFG**: Value Flow Graph construction for tracking value propagation
- **ModRef Analysis**: Modified/Referenced analysis for optimization
- **Call Graph**: Precise indirect call resolution

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

SRAA
----

Strict Relation Alias Analysis built on interprocedural range analysis.

**Location**: ``lib/Alias/SRAA``

**Highlights**:
* Proves ``NoAlias`` results by reasoning about strict inequalities on pointer offsets
* Uses ``InterProceduralRA`` to infer value ranges and propagate constraints
* Tracks primitive layouts of aggregate types to compare sub-field accesses

**Usage**: Register the ``StrictRelations`` pass (``-sraa``) inside an LLVM pass pipeline or via the Lotus AA wrapper.


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



AliasAnalysisWrapper
--------------------

LLVM AliasAnalysis wrapper and integration utilities.

**Location**: ``include/Alias/AliasAnalysisWrapper/``

**Features**: Provides LLVM AA pass integration and result caching.

AserPTA (Advanced Pointer Analysis)
-----------------------------------

Comprehensive pointer analysis framework with multiple context sensitivities.

**Location**: ``include/Alias/AserPTA/``

**Components**:
* **PointerAnalysis/** – Core analysis engine with context sensitivity
* **PreProcessing/** – IR preprocessing and normalization passes
* **Util/** – Analysis utilities and graph writers

**Context Sensitivities**:
* ``NoCtx`` – Context-insensitive analysis
* ``KCallSite`` – K-callsite sensitivity
* ``KOrigin`` – Origin-sensitive analysis

**Solver Types**:
* ``PartialUpdateSolver`` – Incremental constraint solving
* ``WavePropagation`` – SCC-based propagation
* ``DeepPropagation`` – Cycle-aware propagation

LotusAA
-------

Lotus-specific alias analysis framework.

**Location**: ``include/Alias/LotusAA/``

**Components**:
* **Engine/** – Inter/intra-procedural analysis engines
* **MemoryModel/** – Points-to graph and memory modeling
* **Support/** – Configuration and utility functions

**Features**: Modular design for extensible pointer analysis.

UnderApproxAA
-------------

Under-approximate alias analysis for conservative results.

**Location**: ``include/Alias/UnderApproxAA/``

**Components**:
* **Canonical** – Canonical form representations
* **EquivDB** – Equivalence class database
* **UnderApproxAA** – Core under-approximation algorithm

**Features**: Conservative alias analysis for safety-critical applications.
