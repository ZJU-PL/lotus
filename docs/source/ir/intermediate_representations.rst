Intermediate Representations
============================

Advanced IR constructions for program analysis.

ICFG (Interprocedural Control Flow Graph)
-----------------------------------------

Interprocedural control flow representation used as a backbone for many
analyses.

**Location**: ``lib/IR/ICFG/``, ``include/IR/ICFG/``

**Features**:

- Interprocedural edges (call/return)
- Support for context-aware traversals
- Integration with reachability and dataflow analyses

PDG (Program Dependence Graph)
------------------------------

Fine-grained data and control dependence representation.

**Location**: ``lib/IR/PDG/``, ``include/IR/PDG/``

**Core Passes**:

- ``DataDependencyGraph`` – Builds def-use, RAW, and alias-based data edges
- ``ControlDependencyGraph`` – Builds control dependence edges
- ``ProgramDependencyGraph`` – Combines data/control dependencies and exposes
  a query interface

**High-Level Usage**:

PDG is exposed as an LLVM ``ModulePass``. A typical use from C++ code:

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

For interactive querying and slicing, use the :doc:`../pdg_query_language`
and the ``pdg-query`` tool described in :doc:`../tools/analysis`.

SSI (Static Single Information)
-------------------------------

Static single information form as an extension of SSA.

**Location**: ``lib/IR/SSI/``, ``include/IR/SSI/``

**Features**:

- Extended SSA with predicate information
- Better encoding of conditional value relationships
- Basis for advanced optimizations and analyses
