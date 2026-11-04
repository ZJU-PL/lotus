Call Graph Construction
=======================

``CallGraph`` provides call graph construction algorithms for C/C++ programs,
resolving both virtual calls and indirect function pointers.

**Headers**: ``include/Analysis/CallGraph/``

**Implementation**: ``lib/Analysis/CallGraph/``

Overview
--------

This subsystem constructs an interprocedural control flow graph (the Call Graph)
by resolving direct, virtual, and indirect function calls.

Main components
---------------

- ``CallGraph`` and ``CallGraphBuilder``: The foundational classes representing
  and building the call graph.
- ``Resolver``: The abstract interface for resolving indirect and virtual calls.
- ``VirtualCallUtils``: Utilities for identifying and classifying virtual calls.

Algorithms
----------

The module provides multiple algorithms for resolving indirect and virtual calls,
each balancing precision and cost differently:

Virtual Call Resolution (C++)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

These algorithms rely on the Type Hierarchy (see :doc:`type_hierarchy`) to resolve
virtual dispatches:

- **CHA (Class Hierarchy Analysis)**: Statically resolves virtual calls by traversing
  the class inheritance tree without analyzing program control flow.
- **RTA (Rapid Type Analysis)**: Refines CHA by tracking which classes are
  instantiated (identifying ``new`` allocations).
- **VTA (Variable Type Analysis)**: A flow-insensitive algorithm that computes type
  constraints for variables to refine the set of possible targets for a virtual call.
- **OTF (On-The-Fly Call Graph Construction)**: Constructs the call graph on-the-fly
  simultaneously with points-to analysis.

Function Pointer Analysis (FPA) (C/C++)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The FPA subsystem (``lib/Analysis/CallGraph/FPA``) implements function pointer
analyses tailored for indirect call resolution:

- **FLTA**: Flow-insensitive, type-based analysis.
- **MLTA**: Multi-layer type analysis (often used for OS kernels).
- **MLTADF**: Multi-layer type analysis with data flow.
- **KELP**: Context-sensitive kernel-level pointer analysis.

Typical use cases
-----------------

- Build a precise Call Graph to enable interprocedural analysis.
- Resolve indirect calls in C programs or virtual calls in C++ programs.
