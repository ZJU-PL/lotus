==========================
LotusAA — Lotus AA Engine
==========================

Overview
========

LotusAA is the **native alias analysis framework** of Lotus. It provides a
modular engine with **interprocedural**, **flow-sensitive**, and
**field-sensitive** reasoning, designed to integrate tightly with other Lotus
analyses.

* **Location**: ``include/Alias/InclusionBased/LotusAA/``

Components
==========

* **Engine/** – Inter/intra-procedural analysis engines
* **MemoryModel/** – Points-to graph and memory modeling
* **Support/** – Configuration and utility functions

**Features**: strong updates on singleton objects, path-sensitive load-value
recovery, whole-program or module-local operation.

Design
======

LotusAA organizes pointer information into a **points-to graph**:

* Nodes represent **memory objects** and **SSA values**.
* Edges represent **points-to**, **load**, **store**, and **field** relations.
* The graph is updated by a worklist-based solver that processes IR
  instructions according to a set of transfer functions.

The engine can operate in several modes (e.g., whole-program vs. module
local) and is designed to interoperate with higher-level analyses such as
dependence and verification passes.

Path-Sensitive Strong Updates
=============================

LotusAA implements the staged load/store matching algorithm from *Efficient
Strong Updates for Path Sensitive Data Dependence Analysis* (Guo and Zhang,
ICSE 2026). The algorithm prunes overwritten stores before guarded heap
histories are expanded, so loads only see stores that can still define them.

The matching proceeds in stages:

* ``getAliasCondition`` intersects guarded points-to targets directly to
  obtain the may-alias condition of a load/store pair.
* ``areMustAliases`` fingerprints canonical guarded points-to sets and
  confirms hash matches structurally, avoiding collision-based unsoundness.
* Each load reuses the kill forest of its immediate dominating must-alias
  load (its anchor) and considers only stores between the anchor and itself.
* A store kills an older store when their pointers must alias and removing
  the newer store disconnects every older-store-to-load CFG path.
* Only forest roots are expanded by the existing guarded heap walker. This
  retains LotusAA's summary, undef, and confidence handling while avoiding
  conditions for stores already proven dead.

The optimization is controlled by the ``-lotus-enable-must-kill`` flag
(default: true). It follows LotusAA's existing treatment of cyclic CFG
regions: only instructions numbered by the framework's acyclic topological
traversal participate in a must-kill forest.

Usage
=====

LotusAA is exposed through the ``lotus-alias-lotus-aa`` command-line tool, and
provides a modular alias analysis backend for clients such as GVFG and Clam:

* Invoke the standalone driver ``lotus-alias-lotus-aa input.bc``.
* Front-ends can instantiate LotusAA directly or configure its options via flags
  such as ``-lotus-enable-must-kill``.
* Configuration files under ``config/clam/`` control pipeline options when driven
  from Clam.
* Note that LotusAA has its own analysis driver and GVFG integration, rather than
  being selected through the generic ``AliasAnalysisWrapper``.


