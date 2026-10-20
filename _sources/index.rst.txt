Lotus: Program Analysis Framework
==================================

Lotus is a comprehensive toolkit for performing static program analysis, bug detection, verification, and optimization on C/C++ code through the LLVM intermediate representation. The framework provides multiple analysis engines and bug detection tools that can be used individually or in combination to analyze programs for correctness, security vulnerabilities, and optimization opportunities.

The framework is designed with modularity and extensibility in mind, allowing researchers and practitioners to:

- Perform various forms of dataflow analysis (distributive, monotone, pushdown, elimination-based)
- Detect concurrency bugs (data races, deadlocks, atomicity violations)
- Identify memory safety issues (buffer overflows, null pointer dereferences, use-after-free)
- Track information flow through taint analysis
- Verify program properties using abstract interpretation

.. toctree::
   :maxdepth: 2
   :caption: User Guide

   user_guide/architecture
   user_guide/quickstart
   user_guide/major_components
   user_guide/installation
   user_guide/tutorials
   user_guide/bug_detection
   user_guide/pdg_query_language
   user_guide/property_based_slicing
   user_guide/verification_backends
   user_guide/instrumentation_passes
   user_guide/troubleshooting
   tools/index

.. toctree::
   :maxdepth: 2
   :caption: Core Components

   alias/index
   analysis/index
   annotation/index
   apps/index
   cfl/index
   concurrency/index
   dataflow/index
   ir/index
   ml/index
   optimization/index
   security/index
   solvers/index
   symbolic_execution/index
   transform/index
   utils/index
   verification/index
   checker/index

.. toctree::
   :maxdepth: 2
   :caption: Developer Documentation

   developer/api_reference
   developer/developer_guide

Features
--------

* **Multiple Pointer Analysis Algorithms**: DyckAA, Sea-DSA, SparrowAA, AserPTA, TPA, FPA, CFL (via LLVM)
* **Dynamic Analysis Validation**: DynAA for validating static analysis results
* **Intermediate Representations**: PDG, SVFG and its sparse MemorySSA, GVFG, ICFG, ShadowMemSSA, and Gated SSA (GSA)
* **Constraint Solving**: SMT (Z3), BDD (CUDD), WPDS, string constraints (Stingx)
* **Data Flow Analysis Frameworks**: IFDS/IDE framework, Monotone framework, WPDS, and APA (Elimination-based)
* **Bug Detection Checkers**: Integer overflow (Kint), memory safety (AE, Pulse, Saber), concurrency bugs, taint tracking
* **Symbolic Execution**: Path-sensitive engine for bug checking
* **Symbolic Automata**: Seal — FSM model lifting for stateful systems (CAV 2026)
* **LLVM Integration**: Built on LLVM 14 with IR and graph abstractions

Supported Platforms
-------------------

* x86/ARM Linux
* ARM macOS
* LLVM 14.x
* Z3 4.11
* CMake 3.18+
* C++17

Publications
------------

* **CAV 2026**: *Sound and Precise Symbolic Automata Model for Stateful Software Systems*.  
  Xinlong Wu, Ruiyu Zhou, Peisen Yao, and Qingkai Shi.  
  *International Conference on Computer Aided Verification*.

* **ISSTA 2025**: *Program Analysis Combining Generalized Bit-Level and Word-Level Abstractions*  
  Guangsheng Fan, Liqian Chen, Banghu Yin, Wenyu Zhang, Peisen Yao, and Ji Wang.  
  *The ACM SIGSOFT International Symposium on Software Testing and Analysis*.

* **S&P 2024**: *Titan: Efficient Multi-target Directed Greybox Fuzzing*  
  Heqing Huang, Peisen Yao, Hung-Chun Chiu, Yiyuan Guo, and Charles Zhang.

* **USENIX Security 2024**: *Unleashing the Power of Type-Based Call Graph Construction by Using Regional Pointer Information*  
  Yuandao Cai, Yibo Jin, and Charles Zhang.

* **TSE 2024**: *Fast and Precise Static Null Exception Analysis with Synergistic Preprocessing*  
  Yi Sun, Chengpeng Wang, Gang Fan, Qingkai Shi, and Xiangyu Zhang.

* **OOPSLA 2022**: *Indexing the Extended Dyck-CFL Reachability for Context-Sensitive Program Analysis*  
  Qingkai Shi, Yongchao Wang, Peisen Yao, and Charles Zhang.


Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
