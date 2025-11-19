========================
LotusAA — Lotus AA Engine
========================

Overview
========

LotusAA is the **native alias analysis framework** of Lotus. It provides a
modular engine with **interprocedural**, **flow-sensitive**, and
**field-sensitive** reasoning, designed to integrate tightly with other Lotus
analyses.

* **Location**: ``lib/Alias/LotusAA``
* **Components**:
  - ``Engine/`` – intra- and interprocedural drivers
  - ``MemoryModel/`` – points-to graph and memory-object modeling
  - ``Support/`` – configuration, statistics, utilities

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

Usage
=====

LotusAA is typically not run as a standalone tool. Instead, it is selected
via configuration:

* Clam / Lotus front-ends can choose LotusAA as the primary AA engine.
* YAML configurations under ``yaml-configurations/`` and command-line flags
  control whether LotusAA is enabled and how aggressively it runs.

When enabled, LotusAA registers itself with the AA wrapper so that all AA
queries issued by other passes go through its results.


