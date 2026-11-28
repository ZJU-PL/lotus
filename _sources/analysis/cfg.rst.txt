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


Interval reachability index
---------------------------

``Utils/Algorithms/Graph/IntervalReachability.h`` provides a C++17 dense-ID
index for immutable directed graphs. It reuses Lotus's iterative Tarjan SCC
implementation, then unions target-rank intervals along the condensation DAG.
This implementation is inspired by PhASAR's interval index design; it does not
use PhASAR's C++20 graph traits or single-DFS interval builder.

.. code-block:: cpp

   #include "Utils/Algorithms/Graph/IntervalReachability.h"

   lotus::graph::Adjacency graph{{1, 2}, {3}, {3}, {}};
   lotus::graph::IntervalReachabilityIndex index(graph);
   bool yes = index.reaches(0, 3);
   index.forEachReachableTarget(0, [](uint32_t vertex) { /* consume */ });

   // Index only vertices reachable from 1, with vertex 3 as the only target.
   std::vector<bool> targets{false, false, false, true};
   lotus::graph::IntervalReachabilityIndex selected(graph, {1}, targets);

The one-argument constructor indexes all vertices as sources and targets.
The restricted constructor accepts an explicit root list (empty means none);
an omitted/empty target mask selects all indexed vertices. A full-size false
mask selects no targets. Queries for an unindexed source, unselected target or
out-of-range vertex return false. Reachability includes the zero-length path,
so an indexed target reaches itself. Invalid edges/roots or a wrong-sized mask
throw exceptions at construction.

A query costs O(log K) for K intervals of the source SCC. Construction work
depends on the sizes and sorting of merged interval sets; worst-case storage
can be quadratic. Long chains compress to one interval per component. This is
an alternative for repeated structural queries; the existing lazy BFS-based
``CFGReachability`` remains available. It is not a path-feasibility checker or
a context-sensitive call/return matching algorithm. Rebuild after graph changes.
