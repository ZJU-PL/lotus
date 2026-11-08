CFG Analysis
============

Control Flow Graph (CFG) analysis utilities for reachability and structural analysis.

**Headers**: ``include/Analysis/CFG``

**Implementation**: ``lib/Analysis/CFG``

**Main components**:

- **CFGReachability** – Control-flow reachability analysis between basic blocks
- **CodeMetrics** – Code complexity and size metrics
- **Dominator** – Dominator and post-dominator tree construction, with
  ``DominatorForest`` and ``DominatorNode`` providing the forest structures
- **TopologicalOrder** and **SortTopo** – Topological and reverse-topological
  ordering algorithms over the CFG
- **SparseControlFlow** – Instruction-level sparse CFG/ICFG views for
  flow-sensitive clients
- **ExternCallbackModel** – Rewrites callback registrations (metadata,
  ``pthread_create``, and OpenMP) through generated broker functions so static
  call graph construction can see the callback targets
- **GlobalCtorsDtorsModel** – Builds a model that invokes global constructors
  and destructors from a synthetic entry

**Typical use cases**:

- Decide whether a basic block is reachable from another
- Compute loop nests and dominance relationships
- Pre-compute structural information used by subsequent dataflow analyses

**Basic usage (C\+\+)**:

.. code-block:: cpp

   #include <Analysis/CFG/CFGReachability.h>

   llvm::Function &F = ...;
   CFGReachability reach(F);

   llvm::BasicBlock *From = ...;
   llvm::BasicBlock *To   = ...;

   bool reachable = reach.reachable(From, To);

Interpretation
--------------

Reachability is a structural property of the current CFG: it does not account
for path conditions, alias facts, or infeasible branches.  Combine it with
dataflow or symbolic reasoning when a client needs semantic feasibility.
Dominator and topological-order helpers are similarly reusable building
blocks; choose the one that matches the property being computed rather than
reconstructing CFG traversal in each pass.

