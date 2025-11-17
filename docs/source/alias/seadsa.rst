===========================
Sea-DSA — Memory Graph AA
===========================

Overview
========

Sea-DSA is a **context-sensitive, field-sensitive** alias and memory analysis
based on **Data Structure Analysis (DSA)**. It builds **heap graphs** that
summarize program memory and supports modular, interprocedural reasoning.

* **Location**: ``lib/Alias/seadsa``
* **Tools**: ``sea-dsa-dg``, ``seadsa-tool``
* **Use Cases**: Precise mod/ref information, call-graph construction,
  memory-shape reasoning

Memory Graphs
=============

Sea-DSA represents memory as a graph where:

* Nodes correspond to **memory objects** (heap, stack, globals).
* Edges represent **field** and **pointer** relationships.
* Attributes encode properties such as singleton-ness, heap/stack origin,
  and mutation/escape information.

Graphs can be built bottom-up, top-down, and globally, and then merged or
refined through the analysis pipeline.

Usage
=====

Typical command-line usage:

.. code-block:: bash

   ./build/bin/sea-dsa-dg --sea-dsa-dot example.bc
   ./build/bin/seadsa-tool --sea-dsa-dot --outdir results/ example.bc

Key options include:

* ``--sea-dsa-dot`` – emit DOT visualizations of memory graphs.
* ``--sea-dsa-callgraph-dot`` – emit call-graph DOT files.
* ``--outdir <DIR>`` – select output directory for generated artifacts.

Sea-DSA results are consumed by other analyses (e.g., mod/ref, verification
tools) to obtain a precise view of heap structure and aliasing.






