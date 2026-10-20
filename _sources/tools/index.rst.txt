Command-Line Tools Reference
============================

This document provides a comprehensive reference for all command-line tools distributed with the Lotus framework. It covers tool invocation syntax, command-line options, input/output formats, and organization.

Lotus tools are organized into directories under ``tools/``, with corresponding library implementations in ``lib/``. Most tools operate on LLVM bitcode (``.bc``) or LLVM IR (``.ll``) files, while the CFL ``csr`` driver works on graph files and ``lotus-solver-datalog`` consumes JSON/Datalog/Z3 input. All tools are built via CMake subdirectories.

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
     - ``lotus-alias-sparrow-aa``, ``lotus-alias-aser-aa``, ``lotus-alias-dyck-aa``, ``lotus-alias-lotus-aa``, ``lotus-alias-tpa``, ``lotus-alias-gpg``, ``lotus-alias-bootstrap``, ``lotus-alias-fspta``, ``lotus-alias-sea-dsa-dg``, ``lotus-alias-seadsa-tool``, ``lotus-alias-call-graph``, ``lotus-alias-cclyzer-aa``; optional ``dynaa``, ``dynaa-instrument``, ``dynaa-check``, ``dynaa-log-dump`` (``LOTUS_ENABLE_DYNAA``)
   * - ``tools/checker/``
     - ``lotus-check`` (unified binary with ``--engine=generic|kint|taint|ae|saber|pulse|concur|fitx|gsaf|symex``)
   * - ``tools/dataflow/``
     - ``lotus-dfa``, ``lotus-dfa-apa``, ``lotus-dfa-mono``, ``lotus-dfa-ifds``, ``lotus-dfa-npa``, ``lotus-dfa-wpds``
   * - ``tools/optimization/``
     - ``lotus-opt-ipo``, ``lotus-opt-prefetch``, ``lotus-opt-purity``
   * - ``tools/verifier/``
     - ``lotus-verify-symabs-ai``, ``lotus-verify-sifa``, ``lotus-verify-mosaic``, ``clam``, ``seahorn``, ``smack``; SMACK helpers ``llvm2bpl`` and ``extern-statics`` (``LOTUS_ENABLE_SMACK``); optional ``lotus-bool-parse`` and ``lotus-bool-normalize`` (``LOTUS_ENABLE_BOOLEAN_PROGRAM_TOOLS``)
   * - ``tools/ir/``
     - ``lotus-ir-pdg-query``, ``lotus-ir-control-dependence``, ``lotus-ir-usetracessa``, ``lotus-ir-ufg``
   * - ``tools/solver/``
     - ``lotus-solver-datalog``; optional ``lotus-solver-owl`` (``LOTUS_ENABLE_OWL``) and ``lotus-solver-smt-stabilizer`` (``LOTUS_ENABLE_SMT_STABILIZER``)
   * - ``tools/cfl/``
     - ``lotus-cfl-solve``, ``lotus-cfl-alias``, ``lotus-cfl-vf``, ``lotus-cfl-subcubic-aa``, ``lotus-cfl-interleaved-dyck``, ``lotus-cfl-dynamic-dyck``, optional ``csr`` (``LOTUS_ENABLE_CSR``)

Common Configuration Options
----------------------------

Many tools share common output formatting options, selected via configuration files or arguments:

.. list-table:: Output Formats
   :widths: 30 20 50
   :header-rows: 1

   * - Tool
     - Mode
     - Common Arguments
   * - ``lotus-alias-aser-aa``
     - Stats
     - ``-analysis-mode=ci -dump-stats``
   * - ``lotus-check --engine=kint``
     - Text/JSON/SARIF
     - ``--checks=all --report-json=report.json --report-sarif=report.sarif``
   * - ``lotus-check --engine=gsaf``
     - Text/JSON
     - ``--checks=taint,use-after-free --report-json=report.json``
   * - ``lotus-ir-pdg-query``
     - Text/JSON/DOT
     - ``--format=json``
   * - ``lotus-opt-purity``
     - Bitcode/JSON
     - ``-o out.bc --apply-attrs --report-json=purity.json``

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
   ikos
