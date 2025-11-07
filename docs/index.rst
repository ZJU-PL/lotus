Lotus: Program Analysis Framework
==================================

Lotus is a comprehensive program analysis, verification, and optimization framework built on LLVM. It provides multiple toolkits that can be used individually or in combination for various static analysis tasks.

.. toctree::
   :maxdepth: 2
   :caption: Getting Started

   quickstart
   installation

.. toctree::
   :maxdepth: 2
   :caption: Analysis Components

   alias_analysis
   data_flow_analysis
   numerical_abstract_interpretation
   smt_model_checking
   cfl_reachability
   concurrency_bug_checker
   intermediate_representations
   constraint_solving
   slicing_primitives
   fuzzing_support
   utilities

Features
--------

* **Multiple Alias Analysis Algorithms**: DyckAA, CFLAA, Sea-DSA, Andersen, FPA, OriginAA
* **Dynamic Analysis Validation**: DynAA for validating static analysis results
* **Intermediate Representations**: PDG, SVFG, DyckVFG
* **Constraint Solving**: SMT (Z3), BDD (CUDD), WPDS
* **Data Flow Analysis**: IFDS/IDE framework, taint analysis
* **Bug Detection**: Integer overflow, null pointer, buffer overflow detection
* **LLVM Integration**: Built on LLVM 12/14 with comprehensive IR support

Supported Platforms
-------------------

* x86 Linux
* ARM Mac
* LLVM 12.0.0 and 14.0.0
* Z3 4.11

Publications
------------

* **ISSTA 2025**: Program Analysis Combining Generalized Bit-Level and Word-Level Abstractions
  Guangsheng Fan, Liqian Chen, Banghu Yin, Wenyu Zhang, Peisen Yao, and Ji Wang
  The ACM SIGSOFT International Symposium on Software Testing and Analysis

Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
