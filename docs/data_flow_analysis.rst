Data Flow Analysis
===================

Lotus provides IFDS/IDE frameworks and flow-sensitive analysis tools.

IFDS/IDE Framework
------------------

Interprocedural Finite Distributive Subset and Interprocedural Distributive Environment solvers.

**Location**: ``lib/Dataflow``

**Features**: IFDS for subset problems, IDE for environment problems, interprocedural support.

**Usage**:
.. code-block:: cpp

   #include <Dataflow/IFDS.h>
   auto ifdsSolver = std::make_unique<IFDSSolver<Domain>>(problem);

Taint Analysis
--------------

Interprocedural taint analysis using IFDS framework.

**Location**: ``tools/checker/lotus_taint.cpp``

**Usage**:
.. code-block:: bash

   ./build/bin/lotus-taint [options] <input.bc>
   ./build/bin/lotus-taint -sources="read,scanf" -sinks="system,exec" input.bc

**Options**: ``-analysis=<N>``, ``-sources=<funcs>``, ``-sinks=<funcs>``, ``-verbose``

Global Value Flow Analysis (GVFA)
----------------------------------

Value flow analysis for memory safety bugs.

**Location**: ``tools/checker/lotus_gvfa.cpp``

**Usage**:
.. code-block:: bash

   ./build/bin/lotus-gvfa [options] <input.bc>
   ./build/bin/lotus-gvfa -vuln-type=nullpointer input.bc

**Types**: nullpointer (default), taint
**Options**: ``-vuln-type=<type>``, ``-dump-stats``, ``-verbose``

KINT Bug Finder
---------------

Integer and array bounds bug detection.

**Location**: ``tools/checker/lotus_kint.cpp``

**Checks**: overflow, division by zero, bad shifts, array bounds, dead branches.

**Usage**:
.. code-block:: bash

   ./build/bin/lotus-kint -check-all <input.ll>
   ./build/bin/lotus-kint -check-int-overflow -check-array-oob <input.ll>
