==========================
DyckAA — Dyck-CFL Analysis
==========================

Overview
========

DyckAA is a **high-precision, unification-based alias analysis** that uses
Dyck **Context-Free Language (CFL) reachability** to model pointer
relationships. It is designed for **maximum precision** and is well suited to
function-pointer resolution and detailed value-flow reasoning.

* **Location**: ``lib/Alias/UnificationBased/DyckAA``
* **Context**: Context-insensitive
* **Precision**: Field-sensitive, CFL-based alias sets

Core Idea
=========

DyckAA builds a **Dyck graph** whose edges are labeled with operations that
behave like balanced parentheses:

* ``(*`` / ``*)`` — dereference and reference
* ``[field`` / ``field]`` — field access and projection
* Assignment and copy edges

Alias relationships correspond to balanced paths in this labeled graph. The
Dyck-CFL reachability algorithm discovers such paths and **unifies**
equivalent nodes into alias sets.

Algorithm
=========

The analysis uses Dyck Context-Free Language (CFL) reachability to model pointer relationships:

.. code-block:: text

   LLVM IR
      ↓
   [Build Dyck Graph]
      ├─ Nodes: Values (pointers, objects)
      ├─ Edges with labels:
      │  ├─ *(* : Dereference
      │  ├─ *)* : Reference
      │  ├─ *[field]* : Field access
      │  └─ Assignment edges
      ↓
   [Dyck-CFL Reachability]
      ├─ Find balanced paths
      ├─ Unify equivalent nodes
      └─ Build alias sets
      ↓
   [Applications]
      ├─ Alias queries
      ├─ Call graph construction
      ├─ ModRef analysis
      └─ Value flow analysis

Dyck Language
=============

Balanced parentheses language that captures pointer semantics:

- ``( ... )`` : Dereference operations must balance
- ``[ ... ]`` : Field accesses must match
- Paths between nodes indicate aliasing

Example
-------

.. code-block:: c

   int x;
   int *p = &x;    // Edge: p -*)->* x
   int **q = &p;   // Edge: q -*)->* p
   int *r = *q;    // Path: r = *q, q points to p, so r = p
                   // Dyck path: r -*(*-* q -*)->* p

Capabilities
============

DyckAA provides:

* Precise **alias queries** (may/must sets).
* Construction of **call graphs** for indirect calls.
* **ModRef** information (modified/referenced memory).
* **Value-flow graphs** (DyckVFG) for downstream analyses.

Strengths
=========

- Highly precise through CFL reachability
- Handles complex pointer patterns
- Good for function pointer resolution
- Builds precise call graphs

Limitations
===========

- Computationally expensive
- High memory usage for large programs
- Context-insensitive (single analysis per function)

Usage
=====

DyckAA is typically run via its dedicated tool:

.. code-block:: bash

   ./build/bin/lotus-alias-dyck-aa -print-alias-set-info example.bc

Available Options
-----------------

* ``-print-alias-set-info``
  
  Prints the evaluation of alias sets and outputs all alias sets and their
  relations (DOT format).

* ``-count-fp``
  
  Counts how many functions a function pointer may point to.

* ``-function-type-check-level=<0-4>``

  Selects how strict function type compatibility is when resolving pointer
  calls (default ``4``):

  - ``4``: equivalent function types
  - ``3``: same number of parameters and same store size of each parameter
  - ``2``: same number of parameters, comparing only pointer/integer parameters
  - ``1``: same number of parameters
  - ``0``: no compatibility check at all

  FuncTy compatibility also requires that both or neither function is
  variadic, that both or neither has a non-void return value, and that
  ``FuncTy(f1)`` and ``FuncTy(f2)`` cast to each other when
  ``-with-function-cast-comb`` is set.

* ``-dot-dyck-callgraph``
  
  Prints a call graph based on the alias analysis. Can be used with
  ``-with-labels`` option to add labels (call instructions) to the edges in
  call graphs.

Additional flags enable call graph export, function pointer statistics, and
DOT visualizations of internal graphs.

Advanced Features
=================

- **DyckVFG**: Value Flow Graph construction for tracking value propagation
- **ModRef Analysis**: Modified/Referenced analysis for optimization
- **Call Graph**: Precise indirect call resolution
