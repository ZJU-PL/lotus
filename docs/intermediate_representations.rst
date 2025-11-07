Intermediate Representations
===============================

Lotus provides intermediate representations for advanced program analysis.

Program Dependence Graph (PDG)
-------------------------------

Captures data and control dependencies between statements.

**Location**: ``lib/IR/PDG``

**Features**: RAW/WAR/WAW edges, control dependencies, slicing support.

**Usage**:
.. code-block:: cpp

   #include <IR/PDG/ProgramDependenceGraph.h>
   auto pdg = std::make_unique<ProgramDependenceGraph>(function, aliasAnalysis);

Value Flow Graph (VFG)
-----------------------

Tracks value flow and def-use relationships.

**Types**: DyckVFG (with DyckAA), SVFG (sparse, demand-driven).

**Usage**:
.. code-block:: cpp

   #include <Alias/DyckAA/DyckVFG.h>
   auto vfg = dyckAA->getValueFlowGraph();

**Applications**: Null pointer analysis, taint analysis, memory safety.

Call Graph Construction
-----------------------

Builds function call relationships.

**Types**: Static (direct calls), dynamic (runtime), context-sensitive (CFL-based).

**Tools**: ``pdg-query``, ``graspan``

Memory Graph (Sea-DSA)
----------------------

Memory layout and pointer relationships.

**Location**: ``lib/Alias/seadsa``

**Tools**:
.. code-block:: bash

   ./build/bin/sea-dsa-dg --sea-dsa-dot example.bc
   ./build/bin/seadsa-tool --sea-dsa-dot --outdir results/ example.bc

Control Flow Graph (CFG)
------------------------

Enhanced control flow with analysis support.

**Features**: LLVM CFG, ICFG, loop-aware, path profiling (Nisse), fuzzing distances.

Static Single Information (SSI)
-------------------------------

Extension of SSA form (planned).

IR Construction Pipeline
------------------------

1. Load LLVM module
2. Run alias analysis
3. Build representations (PDG, VFG, call graph)
4. Perform analysis
5. Export/visualize results
