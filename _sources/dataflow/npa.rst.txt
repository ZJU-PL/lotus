Newtonian Program Analysis (NPA)
================================

Overview
========

The **Newtonian Program Analysis (NPA)** engine in ``lib/Dataflow/NPA``
implements advanced, research-oriented techniques for compositional and
recurrence-based data-flow reasoning.
It is designed for **numeric** and **relational** analyses that go
beyond classical bit-vector or IFDS/IDE formulations.

* **Location**: ``lib/Dataflow/NPA``
* **Purpose**: library infrastructure for Newton-style program analyses

Conceptual Background
=====================

NPA is based on a sequence of works that recast program analysis as a
form of **Newton iteration** over suitable abstract domains:

* *Newtonian Program Analysis*, JACM 2010.
* *Newtonian Program Analysis via Tensor Product*, POPL 2016.
* *Compositional Recurrence Analysis Revisited*, PLDI 2017.

Roughly, the idea is to:

* encode program semantics as systems of (possibly non-linear)
  recurrence equations,
* apply Newton-like refinement steps over abstract domains,
* obtain high-precision invariants in a compositional way.

Usage Notes
===========

In the current code base, NPA is primarily exposed as an internal
library component.
Clients that use NPA are expected to be familiar with the above papers
and to instantiate the provided abstractions for their specific numeric
domains and recurrence schemes.

This engine is **not** currently wired into a dedicated command-line
tool; instead, it serves as a building block for experimental analyses
within Lotus.


