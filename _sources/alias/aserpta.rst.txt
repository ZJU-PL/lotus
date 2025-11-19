===========================
AserPTA — Pointer Analysis
===========================

Overview
========

AserPTA is a **configurable pointer analysis framework** providing multiple
context sensitivities, memory models, and solver algorithms. It targets
large C/C++ programs and supports both **whole-program** and **modular**
analyses.

* **Location**: ``lib/Alias/AserPTA``
* **Design**: Constraint-based, graph-driven
* **Key Features**:
  - Multiple context sensitivities (CI, K-call-site, K-origin)
  - Field-insensitive and field-sensitive memory models
  - Several solver backends optimized for scalability

Architecture
============

The analysis pipeline is organized as:

1. **IR Preprocessing** (``PreProcessing/``)
   - Canonicalizes GEPs, lowers ``memcpy``, normalizes heap APIs.
   - Removes exception handlers and inserts synthetic initializers.
2. **Constraint Collection** (``PointerAnalysis/``)
   - Extracts five core constraint types:
     ``addr_of``, ``copy``, ``load``, ``store``, ``offset``.
3. **Constraint Graph Construction**
   - Nodes: pointer nodes (CGPtrNode), object nodes (CGObjNode),
     SCC nodes (CGSuperNode).
4. **Solving with Context**
   - Context models: ``NoCtx``, ``KCallSite<K>``, ``KOrigin<K>``.
   - Solver choices: Andersen, WavePropagation, DeepPropagation,
     PartialUpdateSolver.

Analysis Modes and Options
===========================

**Analysis Modes:**
* ``ci``: Context-insensitive (default)
* ``1-cfa``: 1-call-site sensitive
* ``2-cfa``: 2-call-site sensitive  
* ``origin``: Origin-sensitive (tracks thread creation)

**Solver Types:**
* ``basic``: PartialUpdateSolver
* ``wave``: WavePropagation with SCC detection (default)
* ``deep``: DeepPropagation with cycle detection

**Key Options:**
* ``-analysis-mode=<mode>`` – Analysis mode
* ``-solver=<type>`` – Solver algorithm
* ``-field-sensitive`` – Use field-sensitive memory model (default: true)
* ``-dump-stats`` – Print analysis statistics
* ``-consgraph`` – Dump constraint graph to DOT file
* ``-dump-pts`` – Dump points-to sets

Usage
=====

The standalone driver can be invoked as:

.. code-block:: bash

   # Context-insensitive with wave propagation
   ./build/bin/aser-aa input.bc

   # 1-CFA with deep propagation
   ./build/bin/aser-aa -analysis-mode=1-cfa -solver=deep input.bc

   # Origin-sensitive (tracks pthread_create and spawns)
   ./build/bin/aser-aa -analysis-mode=origin input.bc

   # Field-insensitive for faster analysis
   ./build/bin/aser-aa -field-sensitive=false input.bc

In Lotus, AserPTA is also accessible through the AA wrapper and configuration
files that select it as the primary alias analysis engine.


