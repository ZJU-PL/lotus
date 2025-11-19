=====================
Andersen — Inclusion AA
=====================

Overview
========

The Andersen analysis implements a **classic inclusion-based points-to
analysis**. It is **flow-insensitive** and **context-insensitive**, but highly
scalable, making it a good default for **large programs** where speed is more
important than maximum precision.

* **Location**: ``lib/Alias/Andersen``
* **Algorithm**: Andersen-style subset-based pointer analysis
* **Typical Use**: Fast whole-program call-graph and mod/ref precomputation

Constraint System
=================

The analysis builds a constraint graph over program pointers using standard
pointer constraints:

* ``p = &x`` → address-of constraint
* ``p = q`` → copy constraint
* ``p = *q`` → load constraint
* ``*p = q`` → store constraint
* ``p = &x->field`` / GEP → field/offset constraint

After collecting constraints, a worklist-based solver propagates points-to sets
until a fixed point is reached.

Optimizations
=============

The implementation supports several optional optimizations (see the README in
``lib/Alias/Andersen`` for details):

* **HVN / HU** – Hash-based value numbering and Heintze–Ullman style
  equivalence to collapse redundant nodes.
* **HCD / LCD** – Hybrid and lazy cycle detection to identify strongly
  connected components and speed up convergence.

All optimizations are disabled by default and can be enabled via
tool-specific command-line flags.

Usage
=====

The Andersen engine is wrapped as a reusable AA component and also exposed
via a standalone tool:

.. code-block:: bash

   ./build/bin/ander-aa example.bc

In integrated settings (e.g., Clam or LotusAA), it can be selected through
the corresponding configuration files or command-line switches.


