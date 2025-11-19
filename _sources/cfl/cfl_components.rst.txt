CFL Reachability Components
===========================

Advanced CFL reachability algorithms and graph analysis frameworks.

CSIndex (Context-Sensitive Indexing)
-----------------------------------

Context-sensitive indexing for CFL reachability.

**Location**: ``lib/CFL/CSIndex/``

**Features**: Context-aware indexing algorithms for efficient CFL queries.

**Components**:
* Context-sensitive graph indexing
* Reachability query optimization
* Memory-efficient representations

Graspan
-------

Graph analysis framework with parallel processing capabilities.

**Location**: ``lib/CFL/Graspan/``

**Features**:
* **Parallel graph processing** – Multi-threaded analysis
* **Large-scale graphs** – Scalable to millions of nodes
* **CFL algorithms** – Context-free language reachability

**Usage**:
.. code-block:: cpp

   #include <CFL/Graspan/Graspan.h>
   Graspan analyzer(graph);
   auto results = analyzer.analyze();

InterDyckGraphReduce
-------------------

Interprocedural Dyck graph reduction algorithms.

**Location**: ``lib/CFL/InterDyckGraphReduce/``

**Features**: Interprocedural analysis with graph reduction techniques for Dyck languages.

MutualRefinement
---------------

Mutual refinement algorithms for CFL analysis.

**Location**: ``lib/CFL/MutualRefinement/``

**Features**: Bidirectional refinement techniques for improving analysis precision.

POCR (Partial Order Constraint Resolution)
-----------------------------------------

Constraint solving for partial order problems.

**Location**: ``lib/CFL/POCR/``

**Features**:
* **Partial order constraints** – Solving systems of partial order relations
* **Efficient algorithms** – Optimized constraint resolution
* **Graph-based solving** – Constraint graph representations

**Usage**:
.. code-block:: cpp

   #include <CFL/POCR/ConstraintSolver.h>
   ConstraintSolver solver(constraints);
   auto solution = solver.solve();
