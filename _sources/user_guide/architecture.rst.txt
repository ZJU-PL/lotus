Architecture & Major Components
===============================

This page describes the high-level modular structure of the Lotus framework: how source directories are organized, what each major subsystem does, and how the subsystems depend on each other. It is the right starting point for understanding the codebase before diving into a specific component.

Repository Layout
-----------------

The repository is organized into a small number of top-level directories. Every analysis capability is implemented in ``lib/``, exposed through a command-line driver in ``tools/``, and covered by tests under ``tests/``.

.. list-table:: Directory Structure
   :widths: 20 80
   :header-rows: 1

   * - Directory
     - Purpose
   * - ``lib/``
     - All analysis and solver libraries
   * - ``tools/``
     - Command-line tool drivers (one binary per tool)
   * - ``include/``
     - Public headers mirroring ``lib/`` subdirectories
   * - ``tests/``
     - Unit and integration tests
   * - ``third-party/``
     - Bundled third-party libraries (CUDD, WPDS, spdlog)
   * - ``benchmarks/``
     - Benchmark programs for internal evaluation
   * - ``examples/``
     - Optional example programs (controlled by ``LOTUS_BUILD_EXAMPLES``)
   * - ``scripts/``
     - Python helper scripts (organized by subsystem under ``scripts/``)
   * - ``cmake/``
     - CMake helper modules for finding LLVM, Z3, Boost, GTest
   * - ``docs/``
     - Sphinx documentation source

Library Subdirectories (``lib/``)
---------------------------------

The ``lib/`` directory is divided into 16 CMake subdirectories, each of which compiles to one or more static libraries that are linked into the tool binaries.

.. list-table:: Library Components
   :widths: 20 50 30
   :header-rows: 1

   * - Subdirectory
     - Key Libraries / Modules
     - Purpose
   * - ``lib/Alias/``
     - ``DyckAA``, ``AserPTA``, ``SparrowAA``, ``SeaDsaAnalysis``, ``LotusAA``, ``AllocAA``, ``UnderApproxAA``, ``TPA``, ``DemandDriven``, ``InclusionBased``
     - Pointer and alias analysis engines
   * - ``lib/IR/``
     - ``ICFG``, ``PDG``, ``SVFG`` (MemorySSA), ``ShadowMemSSA``, ``GSA``, ``SSI``, ``vSSA``, ``UFG``, ``UseTraceSSA``
     - Graph-based intermediate representations
   * - ``lib/Dataflow/``
     - ``IFDSSolver``, ``IDESolver``, ``IntraMonoSolver``, ``InterMonoSolver``, ``WPDS engine``, ``NPA``
     - Interprocedural dataflow solvers
   * - ``lib/Analysis/``
     - Call graph construction (CHA, RTA, VTA, FPA, OTF), CFG, control dependence, memory, null pointer, purity, SCCP, type hierarchy, feature extraction (``CanaryML``)
     - Foundational analysis utilities
   * - ``lib/Checker/``
     - ``Framework``, ``AE``, ``KINT``, ``Pulse``, ``Saber``, ``Concurrency``, ``GSAF``, ``FiTx``, ``TypeQualifier``, ``Tooling``
     - Bug detection engines and linters
   * - ``lib/Solvers/``
     - Z3 SMT wrapper, CUDD BDD wrapper, WPDS weights
     - Constraint and reachability solvers
   * - ``lib/Verification/``
     - ``SymAbsAI``, ``Sifa``, ``Mosaic``, CLAM abstract interpreter, SeaHorn
     - Formal verification backends
   * - ``lib/CFL/``
     - CFL-reachability graph engines (CSR)
     - Core CFL and pushdown logic
   * - ``lib/Transform/``
     - LLVM IR transformation passes
     - Code canonicalization and lowering
   * - ``lib/Optimization/``
     - ``IPDeadStoreElimination``, SW prefetching
     - Optimization implementations
   * - ``lib/Concurrency/``
     - ``MHP``, ``LockSet``, ``ValueFlow``, ``Thread``, ``OpenMP``, ``MPI``, ``CUDA``, ``LinuxKernel``
     - Concurrency and parallelism analyses
   * - ``lib/Security/``
     - ``ConstantTime``, ``Spectre``, ``LIF``
     - Security-oriented static analyses
   * - ``lib/Fuzzing/``
     - ``CanaryFuzzing``
     - Fuzzing support on top of ``CheckerFramework``
   * - ``lib/SymbolicExecution/``
     - ``CanarySymbolicExecution``
     - Symbolic execution engine
   * - ``lib/Utils/``
     - Shared helper utilities
     - Graph traits, logging, utilities
   * - ``lib/Annotation/``
     - Source-level annotation support
     - Parsers for API specs, taint rules

Tools Directory (``tools/``)
----------------------------

Each subdirectory under ``tools/`` contains the ``main()`` entry points that link against the ``lib/`` libraries and expose them as standalone executables.

.. list-table:: Tool Binaries
   :widths: 20 80
   :header-rows: 1

   * - Subdirectory
     - Executables produced
   * - ``tools/alias/``
     - ``lotus-alias-sparrow-aa``, ``lotus-alias-aser-aa``, ``lotus-alias-dyck-aa``, ``lotus-alias-lotus-aa``, ``lotus-alias-tpa``, ``lotus-alias-bootstrap``, ``lotus-alias-fspta``, ``lotus-alias-sea-dsa-dg``, ``lotus-alias-seadsa-tool``, ``lotus-alias-call-graph``, ``lotus-alias-cclyzer-aa``
   * - ``tools/checker/``
     - ``lotus-check`` (unified binary with ``--engine=generic|kint|ae|taint|pulse|concur|saber|fitx|gsaf|symex``)
   * - ``tools/dataflow/``
     - ``lotus-dfa``, ``lotus-dfa-apa``, ``lotus-dfa-mono``, ``lotus-dfa-ifds``, ``lotus-dfa-npa``, ``lotus-dfa-wpds``
   * - ``tools/optimization/``
     - ``lotus-opt-ipo``, ``lotus-opt-prefetch``, ``lotus-opt-purity``
   * - ``tools/verifier/``
     - ``lotus-verify-symabs-ai``, ``lotus-verify-sifa``, ``lotus-verify-mosaic``, ``clam`` (``clam``, ``clam-pp``, ``clam-diff``), ``seahorn`` (``seahorn``, ``seapp``, ``seainspect``), ``llvm2bpl`` and ``extern-statics`` (SMACK)
   * - ``tools/ir/``
     - ``lotus-ir-pdg-query``, ``lotus-ir-control-dependence``, ``lotus-ir-usetracessa``, ``lotus-ir-ufg``
   * - ``tools/solver/``
     - ``lotus-solver-datalog``, optional ``lotus-solver-owl`` and ``lotus-solver-smt-stabilizer``
   * - ``tools/cfl/``
     - ``lotus-cfl-solve``, ``lotus-cfl-alias``, ``lotus-cfl-vf``, ``lotus-cfl-subcubic-aa``, ``lotus-cfl-interleaved-dyck``, ``lotus-cfl-dynamic-dyck``, optional ``csr``

Intermediate Representations (``lib/IR/``)
------------------------------------------

Lotus constructs several graph-based IRs on top of LLVM IR. These IRs are typically built once and then consumed by multiple analyses.

.. list-table:: Core IRs
   :widths: 20 30 50
   :header-rows: 1

   * - IR
     - Main Classes
     - Purpose
   * - ICFG
     - ``ICFG``, ``ICFGBuilder``, ``ICFGNode``, ``ICFGEdge``
     - Interprocedural control flow; backbone for IFDS/IDE/WPDS
   * - PDG
     - ``ProgramDependencyGraph``, ``ProgramGraph``, ``Node``, ``Edge``
     - Data + control dependence graph
   * - SVFG
     - ``SVFGBuilder``, ``SVFGOPT``, SVFG node types
     - Sparse value-flow graph with Memory SSA integration
   * - DyckVFG
     - ``DyckVFG``
     - Value-flow graph used by DyckAA (located in ``lib/Alias/UnificationBased/DyckAA/``)
   * - GSA
     - ``GateAnalysisPass``, ``GateAnalysis``
     - Gated SSA with gamma/mu/eta nodes

Optional Modules and CMake Flags
--------------------------------

The main optional integrations and build surfaces are controlled by these CMake options:

.. list-table:: CMake Configuration Options
   :widths: 25 15 35 25
   :header-rows: 1

   * - CMake Flag
     - Default
     - Enables
     - Requires
   * - ``LOTUS_ENABLE_CLAM``
     - ``OFF``
     - CLAM abstract interpreter
     - Boost, CRAB
   * - ``LOTUS_ENABLE_SEAHORN``
     - ``OFF``
     - SeaHorn Horn-clause engine
     - Boost
   * - ``LOTUS_ENABLE_SVF``
     - ``OFF``
     - SVF pointer analysis integration
     - SVF install
   * - ``LOTUS_ENABLE_CCLYZER``
     - ``OFF``
     - CclyzerAA Datalog alias analysis
     - Boost
   * - ``LOTUS_BUILD_TESTS``
     - ``OFF``
     - Unit tests under ``tests/``
     - GTest
   * - ``LOTUS_BUILD_EXAMPLES``
     - ``OFF``
     - Example programs under ``examples/``
     - None
   * - ``LOTUS_ENABLE_CFL``
     - ``ON``
     - CFL reachability tools
     - None

Third-Party Libraries
---------------------

Core libraries under ``third-party/`` added in standard builds include:

* **CUDD**: BDD-based symbolic computations used heavily in ``lib/Solvers/``.
* **WPDS**: Weighted pushdown systems; used by the ``lib/Dataflow/`` WPDS engine.
* **spdlog**: A fast C++ logging library used for framework diagnostics.

Additional vendored packages (such as Seal, CLAM, SeaHorn, SMACK, FPsolve, WALi/OpenNWA,
PDAAAL, Horn-ICE, and FlowCutter) are managed as opt-in components gated by their respective
``LOTUS_ENABLE_*`` configuration toggles.
