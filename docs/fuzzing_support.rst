Fuzzing Support
================

Lotus provides directed greybox fuzzing techniques and profiling infrastructure.

Overview
--------

Integrates AFLGo, Hawkeye, DAFL algorithms with Nisse path profiling.

**Location**: ``lib/Apps/Fuzzing/``

**Components**: Directed fuzzing algorithms, distance analysis, target detection, path profiling.

Directed Greybox Fuzzing
-------------------------

**AFLGo (CCS 17)**: Distance-based power scheduling, call graph distances.

**Hawkeye (CCS 18)**: Function-level analysis, rare branch prioritization.

**DAFL (USENIX Security 23)**: Data dependency guided, taint integration.

**Usage**:
.. code-block:: cpp

   #include <Fuzzing/Analysis/AFLGo.hpp>
   auto aflgo = std::make_unique<AFLGoDistance>(module);
   auto distances = aflgo->computeDistances(targetFunctions);

Distance Analysis
-----------------

**Basic Block Distance**: AFLGo distance computation.

**Function Distance**: Hawkeye function-level analysis.

**Data Flow Distance**: DAFL with memory tracking.

Target Detection
----------------

Automatic target identification and instrumentation.

**Usage**:
.. code-block:: cpp

   #include <Fuzzing/Analysis/TargetDetection.hpp>
   auto targets = detector->findTargets(module);

Nisse Path Profiling
-------------------

Ball-Larus edge profiling with loop optimization.

**Features**: MST-based instrumentation, affine variable optimization, reduced overhead.

**Workflow**:
.. code-block:: bash

   bash nisse_profiler.sh program.c
   ./program.profiled  # Run instrumented program
