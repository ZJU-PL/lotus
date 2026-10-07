Data Flow Analysis Frameworks
=============================

Lotus implements four distinct dataflow analysis frameworks, each optimized for different problem classes and precision/performance trade-offs. This page provides an architectural overview of the frameworks, explains when to use each, and documents their core abstractions.

Framework Overview
------------------

Lotus provides four compiled libraries under ``lib/Dataflow/``, each implementing a distinct algorithmic approach:

.. list-table:: Dataflow Engines
   :widths: 15 15 25 25 20
   :header-rows: 1

   * - CMake Target
     - Namespace
     - Primary Solver Classes
     - Key Headers
     - Scope
   * - ``IFDS``
     - ``ifds::``
     - ``IFDSSolver``, ``IDESolver``, ``PathAwareIDESolver``
     - ``IFDSSolver.h``, ``IDESolver.h``
     - Interprocedural
   * - ``MONODataFlow``
     - ``mono::``
     - ``IntraMonoSolver``, ``InterMonoSolver``
     - ``IntraSolver.h``, ``InterSolver.h``
     - Intra/Interprocedural
   * - ``WPDS``
     - ``wpds::``
     - ``InterProceduralDataFlowEngine``
     - ``InterProceduralDataFlow.h``
     - Interprocedural
   * - ``APADataFlow``
     - ``elimination::``
     - ``IntraEliminationSolver``
     - ``IntraEliminationSolver.h``
     - Intraprocedural

When to Use Each Framework
--------------------------

Selecting the right framework depends heavily on the properties of the dataflow problem being solved:

.. list-table:: Framework Selection Guide
   :widths: 15 25 25 35
   :header-rows: 1

   * - Framework
     - Problem Class
     - Context Sensitivity
     - Typical Use Cases
   * - IFDS
     - Distributive (gen-kill, set-union)
     - Call-string (implicit)
     - Taint analysis, uninitialized variables, reaching definitions.
   * - IDE
     - Distributive + value
     - Tabulation + edge
     - Linear analysis, type-state.
   * - Mono (Intra)
     - Arbitrary monotone lattice
     - None (Intraprocedural)
     - Simple lattice problems, worklist fixpoint iteration.
   * - Mono (Inter)
     - Arbitrary monotone lattice
     - K-call-string
     - Constant-prop, context-sensitive analyses.
   * - WPDS
     - Weighted pushdown reachability
     - CFL-reachability
     - Stack-aware analyses, demand-driven queries.
   * - APA
     - Arbitrary meet/transfer
     - None (Intraprocedural)
     - Meet-over-all-paths, path expressions, elimination benchmarks.

The ``lotus-dfa-diff`` tool (located in ``tools/dataflow/``) can be used to perform differential testing across these different engines for a given problem class.

.. toctree::
   :maxdepth: 2
   :caption: Detailed Engine Documentation

   apa
   demand_apa
   control_flow
   mono
   ifds_ide
   vasco
   wpds
   npa
