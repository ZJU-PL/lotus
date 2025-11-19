CFG Analysis
============

Control Flow Graph (CFG) analysis utilities for reachability and structural analysis.

**Headers**: ``include/Analysis/CFG``

**Implementation**: ``lib/Analysis/CFG``

**Main components**:

- **CFGReachability** – Control-flow reachability analysis between basic blocks
- **CodeMetrics** – Code complexity and size metrics
- **Dominator** – Dominator and post-dominator tree construction
- **TopologicalOrder** – Topological ordering algorithms over the CFG

**Typical use cases**:

- Decide whether a basic block is reachable from another
- Compute loop nests and dominance relationships
- Pre-compute structural information used by subsequent dataflow analyses

**Basic usage (C\+\+)**:

.. code-block:: cpp

   #include <Analysis/CFG/CFGReachability.h>

   llvm::Function &F = ...;
   CFGReachability reach(&F);

   llvm::BasicBlock *From = ...;
   llvm::BasicBlock *To   = ...;

   bool reachable = reach.reachable(From, To);


