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
     - Python helper scripts (``clam.py``, ``clam-yaml.py``, etc.)
   * - ``cmake/``
     - CMake helper modules for finding LLVM, Z3, Boost, GTest
   * - ``docs/``
     - Sphinx documentation source

Library Subdirectories (``lib/``)
---------------------------------

The ``lib/`` directory is divided into 14 CMake subdirectories, each of which compiles to one or more static libraries that are linked into the tool binaries.

.. list-table:: Library Components
   :widths: 20 50 30
   :header-rows: 1

   * - Subdirectory
     - Key Libraries / Modules
     - Purpose
   * - ``lib/Alias/``
     - ``DyckAA``, ``AserPTA``, ``SparrowAA``, ``SeaDsaAnalysis``, ``LotusAA``, ``FPA``, ``AllocAA``, ``UnderApproxAA``, ``TPA``
     - Pointer and alias analysis engines
   * - ``lib/IR/``
     - ``ICFG``, ``PDG``, ``SVFG``, ``MemorySSA``, ``DyckVFG``, ``GSA``
     - Graph-based intermediate representations
   * - ``lib/Dataflow/``
     - ``IFDSSolver``, ``IDESolver``, ``IntraMonoSolver``, ``InterMonoSolver``, ``WPDS engine``, ``NPA``
     - Interprocedural dataflow solvers
   * - ``lib/Analysis/``
     - CFG utils, concurrency (MHP, lock sets), range, null pointer, DDA
     - Foundational analysis utilities
   * - ``lib/Checker/``
     - ``GVFAChecker``, ``PulseChecker``, ``AEChecker``, ``SaberChecker``, ``ConcurrencyChecker``, ``Kint``
     - Bug detection engines and linters
   * - ``lib/Solvers/``
     - Z3 SMT wrapper, CUDD BDD wrapper, WPDS weights
     - Constraint and reachability solvers
   * - ``lib/Verification/``
     - CLAM abstract interpreter, SeaHorn, ``SymbolicAbstraction``
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
   * - ``lib/ML/``
     - ``MemoryMLFeaturesPass`` (CanaryML)
     - Machine learning models and features
   * - ``lib/Utils/``
     - Shared helper utilities
     - Graph traits, logging, utilities
   * - ``lib/Annotation/``
     - Source-level annotation support
     - Parsers for API specs, taint rules
   * - ``lib/Apps/``
     - High-level application glue code
     - High-level integration tests

Tools Directory (``tools/``)
----------------------------

Each subdirectory under ``tools/`` contains the ``main()`` entry points that link against the ``lib/`` libraries and expose them as standalone executables.

.. list-table:: Tool Binaries
   :widths: 20 80
   :header-rows: 1

   * - Subdirectory
     - Executables produced
   * - ``tools/alias/``
     - ``lotus-alias-sparrow-aa``, ``lotus-alias-aser-aa``, ``lotus-alias-dyck-aa``, ``lotus-alias-lotus-aa``, ``lotus-alias-tpa``, ``lotus-alias-fpa``, ``lotus-alias-sea-dsa-dg``, ``lotus-alias-seadsa-tool``, ``lotus-alias-call-graph``, ``lotus-alias-cclyzer-aa``
   * - ``tools/checker/``
     - ``lotus-check`` (unified binary with ``--engine=kint|ae|taint|pulse|concur|saber|symex``)
   * - ``tools/dataflow/``
     - ``lotus-dfa``, ``lotus-dfa-apa``, ``lotus-dfa-mono``, ``lotus-dfa-ifds``, ``lotus-dfa-npa``, ``lotus-dfa-wpds``
   * - ``tools/optimization/``
     - ``lotus-opt-ipo``, ``lotus-opt-prefetch``, ``lotus-opt-purity``
   * - ``tools/verifier/``
     - ``lotus-verify-symabs-ai``, ``lotus-verify-sifa``, ``clam``, ``seahorn``, ``smack``
   * - ``tools/ir/``
     - ``lotus-ir-pdg-query``, ``lotus-ir-usetracessa``
   * - ``tools/solver/``
     - ``slot`` (SMT-LIB ↔ LLVM IR)
   * - ``tools/cfl/``
     - CFL-reachability drivers

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
     - ``PDGBuilder``, PDG node/edge types
     - Data + control dependence graph
   * - SVFG
     - ``SVFGBuilder``, ``SVFGOPT``, SVFG node types
     - Sparse value-flow graph with Memory SSA integration
   * - DyckVFG
     - ``DyckVFG``
     - Value-flow graph used by DyckAA
   * - GSA
     - ``GateAnalysisPass``, ``GateAnalysis``
     - Gated SSA with gamma/mu/eta nodes

Optional Modules and CMake Flags
--------------------------------

Three optional external frameworks can be compiled into Lotus, each gated by a CMake option:

.. list-table:: CMake Configuration Options
   :widths: 25 15 35 25
   :header-rows: 1

   * - CMake Flag
     - Default
     - Enables
     - Requires
   * - ``LOTUS_ENABLE_CLAM``
     - ``ON``
     - CLAM abstract interpreter
     - Boost, CRAB
   * - ``LOTUS_ENABLE_SEAHORN``
     - ``ON``
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
     - ``ON``
     - Unit tests under ``tests/``
     - GTest
   * - ``LOTUS_BUILD_EXAMPLES``
     - ``OFF``
     - Example programs under ``examples/``
     - None
   * - ``LOTUS_ENABLE_CFL``
     - ``OFF``
     - CFL reachability tools
     - None

Third-Party Libraries
---------------------

Three libraries are bundled under ``third-party/`` and built unconditionally:

* **CUDD**: BDD-based symbolic computations used heavily in ``lib/Solvers/``.
* **WPDS**: Weighted pushdown systems; used by the ``lib/Dataflow/`` WPDS engine.
* **spdlog**: A fast C++ logging library used for framework diagnostics.
