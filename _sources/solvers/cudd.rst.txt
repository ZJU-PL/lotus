CUDD (Binary Decision Diagrams)
===============================

BDD-based symbolic manipulation and decision procedures for Boolean problems.

Overview
--------

The CUDD backend provides Binary Decision Diagrams (BDDs) to encode and
manipulate Boolean functions and symbolic sets.

**Location**: ``third-party/CUDD/``

Features
--------

- Canonical representation of Boolean functions.
- Efficient Boolean operations (AND, OR, NOT, implication, equivalence).
- Symbolic sets and relations represented as BDDs with projection, union, and intersection.
- Fixed-point iteration over BDDs for reachability and invariance problems.

Typical Use Cases
-----------------

- Symbolic state-space exploration and model checking.
- Boolean abstraction layers in larger analyses.
- Compact representation of large sets or relations.

Basic Usage (C\+\+)
-------------------

.. code-block:: cpp

   #include <CUDD/cudd.h>

   DdManager *Manager = Cudd_Init(0, 0, CUDD_UNIQUE_SLOTS, 127, 0);
   DdNode *X = Cudd_bddIthVar(Manager, 0);
   Cudd_Quit(Manager);

Integration Notes
-----------------

CUDD is used by higher-level applications and analyses that require symbolic
Boolean reasoning (such as predicate relations in NPA and BDD-based points-to
sets). See :doc:`index` for an overview of where CUDD fits in the solver stack.
