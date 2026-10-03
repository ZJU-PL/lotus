Command-Line Tools Reference
============================

This document provides a comprehensive reference for all command-line tools distributed with the Lotus framework. It covers tool invocation syntax, command-line options, input/output formats, and organization.

Lotus tools are organized into directories under ``tools/``, with corresponding library implementations in ``lib/``. All tools operate on LLVM bitcode (``.bc``) or LLVM IR (``.ll``) files as input and are built via CMake subdirectories.

For a feature-oriented walk-through, see :doc:`../user_guide/tutorials` and :doc:`../user_guide/bug_detection`.

Tools by Subdirectory
---------------------

The table below summarizes the major tool subdirectories and the binaries they produce. The documentation is organized to match the ``tools/`` directory structure.

.. list-table:: Tool Subdirectories
   :widths: 20 80
   :header-rows: 1

   * - Subdirectory
     - Executables produced
   * - ``tools/alias/``
     - ``lotus-alias-sparrow-aa``, ``lotus-alias-aser-aa``, ``lotus-alias-dyck-aa``, ``lotus-alias-lotus-aa``, ``lotus-alias-tpa``, ``lotus-alias-fpa``, ``lotus-alias-sea-dsa-dg``, ``lotus-alias-seadsa-tool``, ``lotus-alias-call-graph``, ``lotus-alias-cclyzer-aa``
   * - ``tools/checker/``
     - ``lotus-check`` (unified binary with ``--engine=kint|taint|ae|saber|pulse|concur|fitx|symex``)
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

Common Configuration Options
----------------------------

Many tools share common output formatting options, selected via configuration files or arguments:

.. list-table:: Output Formats
   :widths: 30 20 50
   :header-rows: 1

   * - Tool
     - Mode
     - Common Arguments
   * - ``lotus-aa``
     - CSV/Stats
     - ``-analysis-stats``
   * - ``aser-aa``
     - Stats
     - ``-pta-stats``
   * - ``lotus-gvfa``
     - JSON/Text
     - ``-checker-report-format=json``
   * - ``lotus-kint``
     - Text
     - ``-kint-checks=all``

Detailed Tool Subsections
-------------------------

Explore the individual categories for detailed usage instructions for each tool:

.. toctree::
   :maxdepth: 2

   alias/index
   cfl/index
   checker/index
   dataflow/index
   ir/index
   optimization/index
   solver/index
   verifier/index

Scripting Tools
---------------

.. toctree::
   :maxdepth: 1

   phoenix
