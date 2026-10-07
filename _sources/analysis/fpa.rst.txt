=======================================================
FPA — Function Pointer Analyses (FLTA/MLTA/MLTADF/KELP)
=======================================================

Overview
========

The FPA module implements several **function pointer analysis** algorithms to
resolve indirect calls with different precision/performance trade-offs.

* **Location**: ``lib/Analysis/CallGraph/FPA``
* **Focus**: Indirect call resolution and call-graph construction
* **Algorithms**:
  - **FLTA** (1) – Flow-insensitive, type-based analysis
  - **MLTA** (2) – Multi-layer type analysis
  - **MLTADF** (3) – Multi-layer type analysis with data flow
  - **KELP** (4) – Context-sensitive analysis (USENIX Security'24)

Workflow
========

All FPA variants share a common high-level structure:

1. Scan the program to collect function pointer definitions and uses.
2. Build an abstract model of **types**, **call sites**, and **targets**.
3. Apply the selected algorithm (1–4) to approximate the mapping from call
   sites to possible function targets.
4. Optionally emit diagnostic or visualization output (e.g., call graphs).

Usage
=====

The analyses are exposed through the ``lotus-alias-call-graph`` driver:

.. code-block:: bash

   ./build/bin/lotus-alias-call-graph -cg-type=fpa-flta example.bc  # FLTA
   ./build/bin/lotus-alias-call-graph -cg-type=fpa-mlta -fpa-max-type-layer=10 example.bc  # MLTA

Key Options
-----------

* ``-cg-type=<type>`` – Select analysis algorithm (``fpa-flta``, ``fpa-mlta``, ``fpa-mltadf``, ``fpa-kelp``)
* ``-fpa-max-type-layer=<N>`` – Set maximum type layer for MLTA analysis (default: 10)
* ``-fpa-debug`` – Enable FPA debug output
* ``-fpa-dump-targets=<path>`` – Dump resolved targets to file (use "cout" for standard output)
* ``-S`` – Write call-graph and FPA statistics to standard error

Examples
--------

.. code-block:: bash

   # Using FLTA analysis
   ./build/bin/lotus-alias-call-graph -cg-type=fpa-flta input.bc

   # Using MLTA analysis with output to file
   ./build/bin/lotus-alias-call-graph -cg-type=fpa-mlta -fpa-dump-targets=results.txt input.bc

   # Using KELP analysis with debug info and stats
   ./build/bin/lotus-alias-call-graph -cg-type=fpa-kelp -fpa-debug -S input.bc

FPA results can be consumed directly (for security analyses or refactoring)
or fed into other components that benefit from precise indirect call
resolution.
