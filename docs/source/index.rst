Lotus: Program Analysis Framework
==================================

Lotus is a comprehensive program analysis, verification, and optimization framework built on LLVM. It provides multiple toolkits that can be used individually or in combination for various static analysis tasks.



.. toctree::
   :maxdepth: 1
   :caption: Overview

   major_components
   architecture

.. toctree::
   :maxdepth: 2
   :caption: Getting Started

   quickstart
   installation
   tutorials

.. toctree::
   :maxdepth: 2
   :caption: User Guide

   tools/index
   bug_detection
   pdg_query_language
   troubleshooting

.. toctree::
   :maxdepth: 2
   :caption: Core Components

   alias/index
   analysis/index
   annotation/index
   apps/index
   cfl/index
   dataflow/index
   ir/index
   solvers/index
   transform/index
   utils/index

.. toctree::
   :maxdepth: 2
   :caption: Developer Documentation

   api_reference
   developer_guide

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
