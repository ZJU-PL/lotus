PDG — Program Dependence Graph
==============================

Overview
========

The **Program Dependence Graph (PDG)** is a fine-grained representation of
data and control dependences.
It is built on top of the ICFG and is used for slicing, security analyses,
and other dependence-aware queries.

* **Location**: ``lib/IR/PDG/``, ``include/IR/PDG/``

Core Passes
===========

The PDG infrastructure provides several LLVM passes:

* ``DataDependencyGraph`` – builds def-use, read-after-write (RAW), and
  alias-based **data dependence** edges.
* ``ControlDependencyGraph`` – computes **control dependences** between
  basic blocks and instructions.
* ``ProgramDependencyGraph`` – combines data and control dependences and
  exposes a unified query interface.

High-Level Usage
================

PDG is exposed as an LLVM ``ModulePass``. A typical usage pattern:

.. code-block:: cpp

   #include "IR/PDG/ProgramDependencyGraph.h"
   using namespace llvm;
   using namespace pdg;

   legacy::PassManager PM;
   PM.add(new DataDependencyGraph());
   PM.add(new ControlDependencyGraph());
   auto *pdgPass = new ProgramDependencyGraph();
   PM.add(pdgPass);
   PM.run(module);

   ProgramGraph *G = pdgPass->getPDG();
   // Use G->getNodes(), G->canReach(src, dst), etc.

For interactive querying and slicing, see the :doc:`../pdg_query_language`
and the ``pdg-query`` tool described in :doc:`../tools/analysis`.


