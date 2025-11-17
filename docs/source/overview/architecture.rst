Architecture Overview
=====================

This document provides a comprehensive overview of the Lotus architecture, describing how the different components interact and how they are organized.

System Architecture
-------------------

Lotus is organized into several major subsystems that work together to provide a comprehensive program analysis framework:

.. code-block:: text

   ┌──────────────────────────────────────────────────────────────┐
   │                      Command-Line Tools                       │
   │  (aser-aa, dyck-aa, lotus-aa, lotus-kint, clam, etc.)       │
   └────────────────┬─────────────────────────────────────────────┘
                    │
   ┌────────────────┴─────────────────────────────────────────────┐
   │                    Analysis Applications                      │
   │  - Bug Checkers (Kint, GVFA, Taint, Concurrency)            │
   │  - Abstract Interpreters (CLAM, Sprattus)                    │
   │  - Fuzzing Support (Titan)                                   │
   └────┬────────────┬────────────┬────────────┬─────────────────┘
        │            │            │            │
   ┌────┴────┐  ┌───┴────┐  ┌───┴────┐  ┌────┴─────┐
   │ Alias   │  │  IR    │  │ Data   │  │ Solvers  │
   │ Analysis│  │ (PDG,  │  │ Flow   │  │ (SMT,    │
   │         │  │  VFG)  │  │        │  │  BDD)    │
   └────┬────┘  └───┬────┘  └───┬────┘  └────┬─────┘
        │           │           │            │
   ┌────┴───────────┴───────────┴────────────┴─────────────────┐
   │                    LLVM Infrastructure                      │
   │            (Module, Function, BasicBlock, etc.)            │
   └────────────────────────────────────────────────────────────┘

Core Components
---------------

1. **Alias Analysis** (``lib/Alias/``, ``include/Alias/``)
   
   Multiple pointer analysis algorithms with different precision-performance trade-offs:
   
   - **DyckAA**: Unification-based, exhaustive alias analysis with Dyck-CFL reachability
   - **AserPTA**: Constraint-based pointer analysis with multiple context sensitivities
   - **LotusAA**: Flow-sensitive, field-sensitive interprocedural analysis
   - **Sea-DSA**: Context-sensitive DSA-based memory analysis
   - **Andersen**: Fast context-insensitive inclusion-based analysis
   - **FPA**: Function pointer analysis (FLTA, MLTA, MLTADF, KELP)
   - **SRAA**: Strict Relation Alias Analysis using range analysis
   - **OriginAA**: Origin-sensitive analysis for thread creation tracking
   - **DynAA**: Dynamic validation of static alias analysis results

2. **Intermediate Representations** (``lib/IR/``, ``include/IR/``)
   
   Specialized graph representations for program analysis:
   
   - **PDG (Program Dependence Graph)**: Captures data and control dependencies
   - **DyckVFG (Value Flow Graph)**: Tracks value flow for Dyck-based analyses
   - **Call Graphs**: Multiple call graph implementations (PDG, Dyck, AserPTA)

3. **Data Flow Analysis** (``lib/Dataflow/``, ``include/Dataflow/``)
   
   IFDS/IDE framework for interprocedural data flow problems:
   
   - **Taint Analysis**: Tracks tainted data from sources to sinks
   - **Reaching Definitions**: Identifies definition-use chains
   - **GVFA (Global Value Flow Analysis)**: Tracks value propagation globally

4. **Constraint Solving** (``lib/Solvers/``, ``include/Solvers/``)
   
   Multiple solver backends for different analysis needs:
   
   - **SMT Solver**: Z3-based reasoning for precise constraint solving
   - **BDD Solver**: CUDD-based symbolic set operations
   - **WPDS**: Weighted pushdown systems for interprocedural reachability

5. **Abstract Interpretation** (``lib/Apps/clam/``, ``lib/Analysis/Sprattus/``)
   
   Numerical and symbolic abstract domains:
   
   - **CLAM**: Abstract interpretation with multiple domains (intervals, zones, octagons, polyhedra)
   - **Sprattus**: Configurable abstract interpretation with domain composition

6. **Analysis Utilities** (``lib/Analysis/``, ``include/Analysis/``)
   
   Supporting analysis components:
   
   - **CFG Analysis**: Control flow graph utilities
   - **Concurrency Analysis**: Thread-aware analysis (MHP, lock sets)
   - **Null Pointer Analysis**: Multiple null checking algorithms
   - **Range Analysis**: Value range analysis for integers

7. **Bug Detection** (``lib/Apps/Checker/``)
   
   Security and safety bug detection:
   
   - **Kint**: Integer overflow, division by zero, array bounds checking
   - **GVFA**: Null pointer dereference, use-after-free detection
   - **Taint**: Information flow and taint-style vulnerabilities
   - **Concurrency**: Race conditions and deadlock detection

8. **CFL Reachability** (``lib/CFL/``, ``include/CFL/``)
   
   Context-Free Language reachability engines:
   
   - **Graspan**: Graph-based parallel CFL reachability
   - **CSR**: Compressed sparse row-based CFL solver
   - **POCR**: Partially-ordered CFL reachability

Analysis Flow
-------------

Typical Analysis Pipeline
~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **LLVM IR Input**: Start with LLVM bitcode (``.bc``) or IR (``.ll``)

2. **Preprocessing** (Optional):
   
   - Lower unsupported constructs
   - Canonicalize representations
   - Devirtualization
   - Memory operation normalization

3. **Core Analysis**:
   
   - Build call graph
   - Perform alias analysis
   - Construct intermediate representations (PDG, VFG)
   - Run specialized analyses (taint, null pointer, etc.)

4. **Bug Detection/Verification**:
   
   - Apply checkers
   - Validate properties
   - Generate reports

5. **Output**:
   
   - Text reports
   - JSON/SARIF output
   - DOT graphs for visualization

Example: Null Pointer Detection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   Input: program.bc
      ↓
   [Preprocessing]
      ↓
   [Alias Analysis] → Points-to sets
      ↓
   [Build VFG] → Value flow graph
      ↓
   [Null Flow Analysis] → Track null values
      ↓
   [Check Dereferences] → Identify potential NPDs
      ↓
   Output: Bug report

Pointer Analysis Architectures
-------------------------------

AserPTA Architecture
~~~~~~~~~~~~~~~~~~~~

AserPTA uses a constraint-based approach with multiple solver strategies:

.. code-block:: text

   LLVM IR
      ↓
   [Preprocessing]
      ├─ Canonicalize GEP
      ├─ Lower memcpy
      ├─ Remove exception handlers
      └─ Normalize heap APIs
      ↓
   [Constraint Collection]
      ├─ addr_of: p = &obj
      ├─ copy: p = q
      ├─ load: p = *q
      ├─ store: *p = q
      └─ offset: p = &obj->field
      ↓
   [Constraint Graph]
      ├─ Nodes: Pointers and Objects
      └─ Edges: Constraints
      ↓
   [Solver] (WavePropagation / DeepPropagation)
      ├─ SCC detection
      ├─ Differential propagation
      └─ Cycle elimination
      ↓
   Points-to Sets

LotusAA Architecture
~~~~~~~~~~~~~~~~~~~~

LotusAA provides flow-sensitive, field-sensitive analysis:

.. code-block:: text

   LLVM Module
      ↓
   [Global Analysis]
      ↓
   [Iterative Analysis Loop]
      ├─ [Bottom-Up Traversal]
      │     ├─ Process each function
      │     ├─ Build summaries
      │     └─ Collect interfaces
      │
      ├─ [Call Graph Construction]
      │     ├─ Resolve indirect calls
      │     └─ Update call graph
      │
      └─ [Fixpoint Check]
            ├─ Detect changes
            └─ Reanalyze if needed
      ↓
   Final Points-to Sets + Call Graph

DyckAA Architecture
~~~~~~~~~~~~~~~~~~~

DyckAA uses Dyck-CFL reachability for precise alias analysis:

.. code-block:: text

   LLVM IR
      ↓
   [Build Dyck Graph]
      ├─ Field edges: *[field]*
      ├─ Dereference edges: *(*
      └─ Assignment edges: *()*
      ↓
   [Dyck-CFL Reachability]
      ├─ Compute reachability
      └─ Unify nodes
      ↓
   [Alias Sets]
      ↓
   [Applications]
      ├─ Call graph construction
      ├─ ModRef analysis
      └─ Value flow analysis

Abstract Interpretation Flow (CLAM)
------------------------------------

.. code-block:: text

   LLVM Bitcode
      ↓
   [clam-pp Preprocessing]
      ├─ Lower unsigned comparisons
      ├─ Lower select instructions
      └─ Devirtualization
      ↓
   [CrabIR Translation]
      ├─ Convert LLVM IR to CrabIR
      ├─ Apply memory abstraction
      └─ Integrate heap analysis (Sea-DSA)
      ↓
   [Abstract Interpretation]
      ├─ Select domain (int, zones, oct, pk)
      ├─ Compute fixpoint
      └─ Generate invariants
      ↓
   [Property Checking]
      ├─ Check assertions
      ├─ Check null pointers
      ├─ Check buffer bounds
      └─ Check use-after-free
      ↓
   [Output]
      ├─ Invariants (text/JSON)
      ├─ Verification results
      └─ Warnings/errors

Data Structures
---------------

Points-To Sets
~~~~~~~~~~~~~~

Different analyses use different representations:

- **Sparse BitVector**: Fast set operations (AserPTA)
- **Dense Sets**: Precise membership testing (Andersen)
- **Equivalence Classes**: Unification-based (DyckAA, Sea-DSA)

Constraint Graphs
~~~~~~~~~~~~~~~~~

- **Nodes**: Represent pointers and objects
- **Edges**: Represent constraints (copy, load, store, offset)
- **Super Nodes**: Collapsed SCCs for efficiency

Value Flow Graphs
~~~~~~~~~~~~~~~~~

- **Nodes**: Values and memory locations
- **Edges**: Data flow and pointer propagation
- **Contexts**: Call-site or object sensitivity

Program Dependence Graphs
~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Nodes**: Instructions, functions, parameters
- **Edges**: Data dependencies, control dependencies, parameter bindings
- **Trees**: Hierarchical parameter representations for field sensitivity

Memory Models
-------------

Field-Sensitive
~~~~~~~~~~~~~~~

- Track individual struct fields separately
- More precise but more expensive
- Used by AserPTA (optional), LotusAA, Sea-DSA

Field-Insensitive
~~~~~~~~~~~~~~~~~

- Treat entire objects as single entities
- Faster but less precise
- Used by Andersen, basic AserPTA mode

Flow-Sensitive
~~~~~~~~~~~~~~

- Track different points-to sets at different program points
- Most precise but most expensive
- Used by LotusAA

Flow-Insensitive
~~~~~~~~~~~~~~~~

- Single points-to set per variable across entire program
- Faster, less precise
- Used by most analyses (AserPTA, Andersen, DyckAA)

Context Sensitivity
-------------------

Context-Insensitive
~~~~~~~~~~~~~~~~~~~

- Single analysis for each function
- Fast but imprecise for recursion and callbacks
- Used by Andersen, basic DyckAA

K-CFA (K-Call-Site Sensitive)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- Distinguish calls by up to K call sites on stack
- Good precision-performance trade-off
- Supported by AserPTA (1-CFA, 2-CFA)

Object-Sensitive
~~~~~~~~~~~~~~~~

- Distinguish by allocation site of receiver object
- Good for object-oriented code
- Can be emulated in Lotus

Origin-Sensitive
~~~~~~~~~~~~~~~~

- Track thread creation contexts
- Specialized for concurrent programs
- Supported by AserPTA, OriginAA

Integration Points
------------------

LLVM Pass Integration
~~~~~~~~~~~~~~~~~~~~~

All analyses are implemented as LLVM passes and can be integrated into any LLVM-based tool:

.. code-block:: cpp

   #include "Alias/DyckAA/DyckAliasAnalysis.h"
   
   // Register pass
   PassRegistry &Registry = *PassRegistry::getPassRegistry();
   initializeDyckAliasAnalysisPass(Registry);
   
   // Use in pass manager
   legacy::PassManager PM;
   PM.add(new DyckAliasAnalysis());
   PM.run(module);

Standalone Tools
~~~~~~~~~~~~~~~~

Each major component has standalone command-line tools:

- ``aser-aa``, ``dyck-aa``, ``lotus-aa``: Alias analysis
- ``lotus-kint``, ``lotus-gvfa``, ``lotus-taint``: Bug detection
- ``clam``, ``clam-pp``, ``clam-diff``: Abstract interpretation
- ``pdg-query``: PDG queries

Python Integration
~~~~~~~~~~~~~~~~~~

Python scripts provide high-level interfaces:

- ``clam.py``: Full analysis pipeline with compilation
- ``clam-yaml.py``: Configuration-based analysis
- ``seapy``: SeaHorn integration

Performance Considerations
--------------------------

Scalability Strategies
~~~~~~~~~~~~~~~~~~~~~~

1. **Context Pruning**: Limit context depth (K in K-CFA)
2. **SCC Collapsing**: Merge strongly connected components
3. **Lazy Evaluation**: Compute results on demand
4. **Incremental Analysis**: Reuse results across runs
5. **Parallel Processing**: Some analyses support parallelization (Graspan)

Precision vs. Performance
~~~~~~~~~~~~~~~~~~~~~~~~~

From fastest to most precise:

1. **Andersen** (context-insensitive, field-insensitive)
2. **AserPTA-CI** (context-insensitive, field-sensitive)
3. **AserPTA-1CFA** (1-call-site sensitive)
4. **DyckAA** (unification-based, field-sensitive)
5. **LotusAA** (flow-sensitive, field-sensitive)

Recommended Configurations
~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Quick Scan**: Andersen or AserPTA-CI
- **Balanced**: AserPTA 1-CFA with wave propagation
- **Precise**: DyckAA or LotusAA
- **Verification**: CLAM with polyhedra domain

Extension Points
----------------

Adding New Analyses
~~~~~~~~~~~~~~~~~~~

1. Create LLVM ModulePass or FunctionPass
2. Implement ``runOnModule()`` or ``runOnFunction()``
3. Query existing analyses (AA, PDG, etc.)
4. Register pass in ``CMakeLists.txt``
5. Add tool in ``tools/`` directory

Adding New Checkers
~~~~~~~~~~~~~~~~~~~

1. Extend ``BugDetectorPass`` base class
2. Implement checker logic
3. Report bugs via ``BugReportMgr``
4. Add to ``lotus-kint`` or create new tool

Adding New Abstract Domains
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. Extend CRAB domain interface
2. Implement lattice operations
3. Register with CLAM
4. Add command-line option

See :doc:`../developer/developer_guide` for detailed instructions.

