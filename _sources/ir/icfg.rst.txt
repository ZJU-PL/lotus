ICFG — Interprocedural Control Flow Graph
=========================================

Overview
========

The **Interprocedural Control Flow Graph (ICFG)** is the primary control-flow
representation used by many analyses in Lotus.
It extends the standard intraprocedural CFG with **call** and **return**
edges to capture interprocedural control flow.

* **Location**: ``lib/IR/ICFG/``, ``include/IR/ICFG/``

Key Features
============

* Interprocedural edges (call/return) connecting caller and callee code.
* Support for context-aware traversals used by dataflow and reachability
  engines.
* Integration with higher-level analyses such as IFDS/IDE, WPDS, and PDG
  construction.

The ICFG serves as a backbone graph on top of which many of the dataflow
engines in Lotus operate.


