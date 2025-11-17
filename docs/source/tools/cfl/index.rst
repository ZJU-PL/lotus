CFL Tools
=========

This page documents the CFL-related tools under ``tools/cfl/``. For the
underlying theory and components, see :doc:`../../analysis/cfl_reachability`.

CSR: Context-Sensitive Reachability
-----------------------------------

Indexing-based context-sensitive reachability engine for large graphs.

**Binary**: ``csr``  
**Location**: ``tools/cfl/csr/csr.cpp``

CSR operates on graph files (not LLVM bitcode directly) and answers reachability
queries with different indexing strategies (GRAIL, PathTree, or combined).

**Basic Usage**:

.. code-block:: bash

   ./build/bin/csr [options] graph_file

**Common Options** (see ``tools/cfl/csr/README.md`` for full list):

- ``-m <method>`` – Indexing method:

  - ``pathtree`` – PathTree indexing
  - ``grail`` – GRAIL labeling
  - ``pathtree+grail`` – Combined approach

- ``-t`` – Evaluate transitive closure
- ``-r`` – Evaluate tabulation algorithm
- ``-p`` – Evaluate parallel tabulation algorithm
- ``-j <N>`` – Number of threads for parallel tabulation (0 = auto)
- ``-g <file>`` – Generate queries and save to file
- ``-q <file>`` – Load queries from file

**Examples**:

.. code-block:: bash

   # GRAIL-based reachability
   ./build/bin/csr input.graph

   # PathTree indexing
   ./build/bin/csr -m pathtree input.graph

   # Parallel tabulation with 4 threads
   ./build/bin/csr -p -j 4 input.graph

Graspan: Disk-Based Parallel Graph System
-----------------------------------------

Graspan is a disk-based parallel graph engine for computing dynamic transitive
closure on large program graphs. Lotus bundles the C++ implementation.

**Binary**: ``graspan``  
**Location**: ``tools/cfl/graspan/graspan.cpp``

Graspan operates on edge-list graphs and grammar files describing CFL rules.

**Input Format**:

- **Graph file** (edge list):

  .. code-block:: text

     <src> <dst> <label>

- **Grammar file** (CNF rules):

  .. code-block:: text

     A B C   # A → B C

**Basic Usage**:

.. code-block:: bash

   ./build/bin/graspan <graph_file> <grammar_file> <num_partitions> <mem_gb> <num_threads>

See ``tools/cfl/graspan/README.md`` and the original Graspan documentation for
details on configuration and performance tuning.

POCR: Partially-Ordered CFL Reachability
----------------------------------------

POCR provides a family of tools for partially-ordered CFL reachability-based
analyses. The main driver is split into several frontends in ``tools/cfl/pocr/``.

**Location**: ``tools/cfl/pocr/``

Sub-Tools
~~~~~~~~~

Each sub-tool has its own ``main`` and focuses on a specific analysis:

- ``pocr-aa`` (``tools/cfl/pocr/AA/aa.cpp``)

  - CFL-based alias/points-to analysis on CFL-encoded graphs.

- ``pocr-cfl`` (``tools/cfl/pocr/CFL/cfl.cpp``)

  - Generic CFL reachability solver over graph + grammar inputs.

- ``pocr-fp`` (``tools/cfl/pocr/FoldablePattern/fp.cpp``)

  - Function-pointer-oriented analysis via CFL reachability on call graphs.

- ``pocr-vf`` (``tools/cfl/pocr/VFA/vf.cpp``)

  - Value-flow analysis based on CFL reachability.

**Typical Usage Pattern**:

1. Build a graph and grammar representing the analysis problem
2. Run the corresponding POCR tool:

   .. code-block:: bash

      # Alias analysis variant
      ./build/bin/pocr-aa <graph> <grammar> [options]

      # Generic CFL queries
      ./build/bin/pocr-cfl <graph> <grammar> [options]

      # Function pointer analysis
      ./build/bin/pocr-fp <graph> <grammar> [options]

      # Value flow analysis
      ./build/bin/pocr-vf <graph> <grammar> [options]

**When to Use CSR vs. Graspan vs. POCR**
----------------------------------------

- **CSR**:

  - When you already have a graph representation and want fast reachability
    queries with indexing.
  - Good for experimentation with different indexing schemes and parallel
    tabulation.

- **Graspan**:

  - When your graphs and grammars are very large and disk-based processing is
    required.
  - Focused on dynamic transitive closure for program analyses at scale.

- **POCR**:

  - When you want to encode alias, value-flow, or function-pointer problems as
    CFL reachability and experiment with partially-ordered variants.

